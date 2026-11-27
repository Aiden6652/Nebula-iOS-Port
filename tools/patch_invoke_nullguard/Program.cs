using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R123] 修 il2cpp_runtime_invoke(IntPtr method=0, ...) 的 SIGSEGV。
//
// 真机 1.9MB 日志（main BUILD=66cb9e001f2d，即 R122 版）崩栈：
//   NebulaPlugin.LoadInternal -> SceneManager:get_sceneLoaded ->
//   Il2CppObjectPool:Get -> System.Object:Initializer<UnityAction`2> ->
//   UnityEngine.Events.UnityAction`2:.cctor -> Il2CppSystem.Type:internal_from_handle ->
//   IL2CPP:il2cpp_runtime_invoke(method=0) -> SIGSEGV.
//   native fault: ldr w8,[x8,#8] (method->methodPointer, method=NULL)。
//
// 根因：Il2CppInterop 为泛型委托 UnityAction`2 生成的 interop 类，其 cctor 里
//   internal_from_handle 要 invoke 的 IL2CPP 侧方法指针为 0（该类/方法在 IL2CPP 侧
//   不存在或被裁剪/版本不匹配）。il2cpp_runtime_invoke 是裸 P/Invoke、无 null 检查，
//   把 0 传进 libil2cpp -> 解引用 method->methodPointer -> SIGSEGV。
//   这与 R122 的 il2cpp_runtime_class_init(0) 完全同根因族；R119 DEFER 只拦
//   RunClassConstructor icall，拦不住 JIT 内部自动跑的 cctor（R122 banner 已记录）。
//
// 修法（真修复·参数有效性守卫·非绕过）：
//   把 IL2CPP.il2cpp_runtime_invoke 的 P/Invoke 改名为 _Raw 并保留 native
//   EntryPoint；新建同名托管 wrapper：method==IntPtr.Zero 时返回 IntPtr.Zero
//   （no-op），否则调 _Raw 原样透传全部参数。
//   - 语义正确：method==0 表示 IL2CPP 侧根本没有该方法可 invoke，调用不可能产生
//     有效结果；返回 null 句柄是唯一安全语义（与 R122 class_init no-op 同类）。
//   - 只对 null 第一参数生效，任何 AU 版本/插件组合/设备/iOS 版本行为一致。
//   - 跨程序集调用点（设备 Documents/BepInEx/interop/*.dll 里生成的 interop 类）
//     按名字+签名自动绑到新 wrapper，设备 DLL 零改动。
// 幂等：已有 _Raw 则跳过；原子写回（R78 截断教训）。
class PatchInvokeNullGuard
{
    const string TYPE = "Il2CppInterop.Runtime.IL2CPP";
    const string METHOD = "il2cpp_runtime_invoke";
    const string RAW = METHOD + "_Raw";
    static int patched = 0, skipped = 0;

    static AssemblyDefinition Load(string path) => AssemblyDefinition.ReadAssembly(path,
        new ReaderParameters { ReadingMode = ReadingMode.Immediate, InMemory = true,
                               AssemblyResolver = new DefaultAssemblyResolver() });

    // 把 module 内所有 operand 指向 m 的 call/callvirt 重定向到 wrapper（跳过 wrapper 自身）
    static void RedirectCalls(ModuleDefinition module, MethodDefinition m, MethodDefinition wrapper)
    {
        int n = 0;
        foreach (var t in module.GetTypes())
            foreach (var mm in t.Methods)
            {
                if (mm.Body == null || mm == wrapper) continue;
                foreach (var ins in mm.Body.Instructions)
                    if ((ins.OpCode == OpCodes.Call || ins.OpCode == OpCodes.Callvirt)
                        && ReferenceEquals(ins.Operand, m))
                    {
                        ins.Operand = wrapper;
                        n++;
                    }
            }
        if (n > 0) Console.WriteLine("[r123]   同模块调用点重定向 " + n + " 处 -> wrapper");
    }

    static void PatchOne(ModuleDefinition mod, TypeDefinition type, MethodDefinition m)
    {
        // 期望签名: IntPtr il2cpp_runtime_invoke(IntPtr method, IntPtr obj, void** param, ref IntPtr exc)
        if (m.ReturnType.MetadataType != MetadataType.IntPtr) {
            Console.WriteLine("[r123] !! 返回类型非 IntPtr，跳过: " + m.FullName);
            skipped++; return;
        }
        if (m.Parameters.Count != 4) {
            Console.WriteLine("[r123] !! 参数数量非 4，跳过: " + m.FullName);
            skipped++; return;
        }
        if (m.Parameters[0].ParameterType.MetadataType != MetadataType.IntPtr) {
            Console.WriteLine("[r123] !! 第一参数非 IntPtr，跳过: " + m.FullName);
            skipped++; return;
        }
        if (!m.IsPInvokeImpl) {
            Console.WriteLine("[r123] !! 非 P/Invoke，跳过: " + m.FullName);
            skipped++; return;
        }

        // 1) 原 P/Invoke 改名 _Raw，保留 native 导出名（关键：否则 dlsym 找不到）
        string nativeEntry = m.PInvokeInfo != null && !string.IsNullOrEmpty(m.PInvokeInfo.EntryPoint)
            ? m.PInvokeInfo.EntryPoint : METHOD;
        m.Name = RAW;
        if (m.PInvokeInfo != null) m.PInvokeInfo.EntryPoint = nativeEntry;

        // 2) 新建同名 wrapper：if (method==0) return IntPtr.Zero; else return _Raw(...);
        var w = new MethodDefinition(METHOD,
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            mod.TypeSystem.IntPtr);
        // 同步 unsafe 签名：param 是指针类型，exc 是 byref
        foreach (var p in m.Parameters)
            w.Parameters.Add(new ParameterDefinition(p.ParameterType));

        var body = new MethodBody(w) { InitLocals = false };
        w.Body = body;
        var il = body.GetILProcessor();

        // method==0 -> 返回 IntPtr.Zero；否则重压 4 个参数调 _Raw 并原样返回。
        // brfalse 弹栈；跳转目标处重新构造返回值（ldc.i4.0 + conv.i = IntPtr.Zero）。
        var retNull = il.Create(OpCodes.Ldc_I4_0);
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Brfalse_S, retNull);  // method == 0 -> no-op
        il.Emit(OpCodes.Ldarg_0);             // 重新压参数（brfalse 已弹栈）
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Ldarg_2);
        il.Emit(OpCodes.Ldarg_3);
        il.Emit(OpCodes.Call, m);             // 调 _Raw
        il.Emit(OpCodes.Ret);
        il.Append(retNull);
        il.Emit(OpCodes.Conv_I);              // 0 -> native int (IntPtr.Zero)
        il.Emit(OpCodes.Ret);

        type.Methods.Add(w);

        // 3) 同模块内调用点重定向
        RedirectCalls(mod, m, w);

        patched++;
        Console.WriteLine("[r123] " + METHOD + "(method,obj,param,exc) -> null-method 守卫 wrapper（native EntryPoint=" + nativeEntry + "）");
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_invoke_nullguard <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = argv[0];
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            var t = mod.GetTypes().FirstOrDefault(x => x.FullName == TYPE);
            if (t == null) throw new Exception("找不到 " + TYPE);

            var raw = t.Methods.FirstOrDefault(x => x.Name == RAW);
            var wrapper = t.Methods.FirstOrDefault(x => x.Name == METHOD && !x.IsPInvokeImpl && x.HasBody);
            var orig = t.Methods.FirstOrDefault(x => x.Name == METHOD && x.IsPInvokeImpl && x.IsStatic
                && x.ReturnType.MetadataType == MetadataType.IntPtr
                && x.Parameters.Count == 4
                && x.Parameters[0].ParameterType.MetadataType == MetadataType.IntPtr);
            // 幂等：wrapper + _Raw 都在、且无残留同名 P/Invoke = 已注入
            if (raw != null && wrapper != null && orig == null) {
                Console.WriteLine("[r123] 已注入（存在 " + RAW + " 与同名 wrapper），跳过");
                asm.Dispose();
                return 0;
            }
            if (orig == null) throw new Exception("找不到 " + METHOD + "(4 args)->IntPtr P/Invoke，且未注入");
            if (raw != null || wrapper != null) {
                Console.WriteLine("[r123] !! 部分注入状态（raw=" + (raw!=null) + " wrapper=" + (wrapper!=null) + "），拒绝修改");
                asm.Dispose(); return 1;
            }

            PatchOne(mod, t, orig);

            string tmp = path + ".r123tmp";
            asm.Write(tmp);
            asm.Dispose();
            File.Copy(tmp, path, true);
            File.Delete(tmp);
            Console.WriteLine("[r123] 完成，改写 " + patched + " 个方法，跳过 " + skipped + " 个");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("[r123] !! 补丁失败: " + e); return 1; }
    }
}
