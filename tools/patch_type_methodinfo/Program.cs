// R131: 根治 UnityAction`2.cctor -> get_TypeHandle(null) NRE（iOS 预生成 interop 专属问题）。
//   真机 R130：PatchAll 跑满 seq=163 ok=160（detour 已健康），但 NOS 加载崩在
//     SceneManager.get_sceneLoaded() -> Il2CppObjectPool.Get<UnityAction`2>
//       -> UnityAction`2..cctor() -> MakeGenericType(...)=null -> get_TypeHandle(null) -> NRE.
//   根因：Il2CppSystem.Type.cctor 里所有 NativeMethodInfoPtr_* 都是用
//     GetIl2CppMethodByToken(CPS<Type>.NativeClassPtr, <烤死的 token>) 解析的。
//   预生成 interop 的 token 在 iOS GameAssembly 里错位 -> MakeGenericType 的
//   NativeMethodInfoPtr=0 -> runtime_invoke(0) 被 R123 guard 转 null -> 坏链。
//   真修（真修复·非绕过·跨 AU 版本/设备/iOS 通用）：按【方法名】解析而非 token。
//     il2cpp_class_get_method_from_name(class, "MakeGenericType", 1)
//     il2cpp_class_get_method_from_name(class, "get_TypeHandle", 0)
//   方法名在所有 AU 版本不变、与 token 无关；两个 native 函数签名都是
//   (IntPtr klass, ...)->IntPtr，栈顶消耗/产出一律，可安全替换。
//   含完整 IL 栈平衡校验，栈冲突 exit 1（R126b 非法 IL 教训）。幂等、原子写回。
using System;
using System.Collections.Generic;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    // 从模块内任意指令里找 Il2CppInterop.Runtime.IL2CPP 的 TypeReference（带正确 Scope）
    static TypeReference il2cppClassRef(ModuleDefinition mod)
    {
        foreach (var t in mod.GetTypes())
            foreach (var md in t.Methods)
            {
                if (!md.HasBody) continue;
                foreach (var i in md.Body.Instructions)
                    if (i.Operand is MethodReference mr && mr.DeclaringType != null &&
                        mr.DeclaringType.FullName == "Il2CppInterop.Runtime.IL2CPP")
                        return mr.DeclaringType;
            }
        return null;
    }

    static MethodReference makeGetByName(ModuleDefinition mod, TypeReference il2cppClass)
    {
        var m = new MethodReference("il2cpp_class_get_method_from_name", mod.TypeSystem.IntPtr, il2cppClass);
        m.HasThis = false;
        m.Parameters.Add(new ParameterDefinition(mod.TypeSystem.IntPtr));  // klass
        m.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));  // name
        m.Parameters.Add(new ParameterDefinition(mod.TypeSystem.Int32));   // paramCount
        return m;
    }

    // 单条指令的栈净变化（本 patch 片段只涉及 ldsfld/ldstr/ldc.i4/call/stsfld，全部覆盖）
    static void stackDelta(Instruction ins, out int pop, out int push)
    {
        pop = 0; push = 0;
        switch (ins.OpCode.Code)
        {
            case Code.Ldsfld: case Code.Ldstr:
            case Code.Ldc_I4_0: case Code.Ldc_I4_1: case Code.Ldc_I4: case Code.Ldc_I4_S:
                push = 1; break;
            case Code.Stsfld:
                pop = 1; break;
            case Code.Call: case Code.Callvirt:
            {
                var mr = ins.Operand as MethodReference;
                if (mr != null)
                {
                    pop = mr.Parameters.Count + (mr.HasThis ? 1 : 0);
                    push = mr.ReturnType != null && mr.ReturnType.FullName != "System.Void" ? 1 : 0;
                }
                break;
            }
            default:
                throw new Exception("片段含未建模 opcode: " + ins.OpCode.Code);
        }
    }

    // 校验被替换的 ldsfld..stsfld 片段栈平衡：入口0、中间不下溢、出口0。
    static string verifySegment(Instruction start, Instruction end)
    {
        int stack = 0;
        var cur = start;
        while (cur != null)
        {
            int pop, push;
            stackDelta(cur, out pop, out push);
            stack -= pop;
            if (stack < 0) return "栈下溢 @ " + cur;
            stack += push;
            if (cur == end) break;
            cur = cur.Next;
            if (cur == null) return "片段未闭合（未到 stsfld）";
        }
        if (stack != 0) return "片段出口栈深度=" + stack + " (期望0)";
        return null;
    }

    static int patchCctor(ModuleDefinition mod, MethodDefinition cctor)
    {
        var il2cppClass = il2cppClassRef(mod);
        if (il2cppClass == null) { Console.Error.WriteLine("[R131] x 找不到 Il2CppInterop.Runtime.IL2CPP 类型引用"); return 4; }
        var getByName = makeGetByName(mod, il2cppClass);

        // 目标：field 名前缀 -> (方法名, 参数个数)
        var targets = new Dictionary<string, string[]>
        {
            { "NativeMethodInfoPtr_MakeGenericType_Public", new[]{"MakeGenericType","1"} },
            { "NativeMethodInfoPtr_get_TypeHandle_Public_Virtual", new[]{"get_TypeHandle","0"} },
        };

        var body = cctor.Body;
        var ilp = body.GetILProcessor();
        var ins = body.Instructions;
        int patched = 0;

        foreach (var kv in targets)
        {
            for (int i = 0; i < ins.Count; i++)
            {
                var st = ins[i];
                if (st.OpCode != OpCodes.Stsfld) continue;
                var fld = st.Operand as FieldReference;
                if (fld == null || !fld.Name.StartsWith(kv.Key)) continue;

                // 序列: [i-3] ldsfld CPS<Type>.NativeClassPtr ; [i-2] ldc.i4 token ; [i-1] call GetIl2CppMethodByToken ; [i] stsfld
                if (i < 3) { Console.Error.WriteLine("[R131] x " + kv.Key + " stsfld 太靠前"); return 5; }
                var callIns = ins[i - 1];
                var ldcIns = ins[i - 2];
                if (callIns.OpCode != OpCodes.Call || !(callIns.Operand is MethodReference cm) ||
                    cm.Name != "GetIl2CppMethodByToken")
                {
                    // 可能已 patch（call 已是 il2cpp_class_get_method_from_name）-> 幂等跳过
                    if (callIns.OpCode == OpCodes.Call && callIns.Operand is MethodReference cm2 &&
                        cm2.Name == "il2cpp_class_get_method_from_name") continue;
                    Console.Error.WriteLine("[R131] x " + kv.Key + " 前置 call 非 GetIl2CppMethodByToken: " + callIns.Operand);
                    return 6;
                }
                if (ldcIns.OpCode != OpCodes.Ldc_I4 && ldcIns.OpCode != OpCodes.Ldc_I4_S)
                {
                    Console.Error.WriteLine("[R131] x " + kv.Key + " token 非 ldc.i4"); return 7;
                }

                // 片段起点 = ldsfld CPS<Type>.NativeClassPtr（i-3），终点 = stsfld（i）
                var segStart = ins[i - 3];
                if (segStart.OpCode != OpCodes.Ldsfld)
                {
                    Console.Error.WriteLine("[R131] x " + kv.Key + " 片段起点非 ldsfld"); return 9;
                }

                // 替换 ldc.i4 token -> ldstr name + ldc.i4 paramCount + call getByName
                var ldstr = ilp.Create(OpCodes.Ldstr, kv.Value[0]);
                var ldcCnt = kv.Value[1] == "0" ? ilp.Create(OpCodes.Ldc_I4_0) : ilp.Create(OpCodes.Ldc_I4_1);
                var callByName = ilp.Create(OpCodes.Call, getByName);
                ilp.InsertBefore(ldcIns, ldstr);
                ilp.InsertBefore(ldcIns, ldcCnt);
                ilp.InsertBefore(ldcIns, callByName);
                ilp.Remove(ldcIns);   // 移除 token
                ilp.Remove(callIns);  // 移除 GetIl2CppMethodByToken

                // 校验替换后片段栈平衡（R126b 教训：改动片段必须栈平衡）
                string segErr = verifySegment(segStart, st);
                if (segErr != null)
                {
                    Console.Error.WriteLine("[R131] x " + kv.Key + " 片段栈校验失败: " + segErr); return 8;
                }
                Console.WriteLine("[R131]   √ " + kv.Key + " -> get_method_from_name(\"" + kv.Value[0] + "\", " + kv.Value[1] + ") [栈平衡OK]");
                patched++;
                break; // 每类只 patch 一处
            }
        }

        if (patched == 0) { Console.WriteLine("[R131] 已全部 patch（幂等跳过）"); return 0; }
        Console.WriteLine("[R131]   √ 全部改动片段 IL 栈平衡校验通过");
        return 0;
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_type_methodinfo <Il2Cppmscorlib.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppSystem.Type");
        if (type == null) { Console.Error.WriteLine("[R131] x 找不到 Il2CppSystem.Type"); return 3; }
        var cctor = type.Methods.FirstOrDefault(m => m.Name == ".cctor" && m.IsStatic);
        if (cctor == null || !cctor.HasBody) { Console.Error.WriteLine("[R131] x 找不到 Il2CppSystem.Type..cctor"); return 3; }

        int rc = patchCctor(mod, cctor);
        if (rc != 0) return rc;
        mod.Write(path);
        Console.WriteLine("[R131] √ Type.cctor methodInfo 按名字解析补丁已写回");
        return 0;
    }
}
