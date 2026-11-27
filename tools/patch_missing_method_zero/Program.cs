using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R124a] 根治 il2cpp_runtime_invoke 坏 method 指针崩溃（UnityAction`2 cctor）。
//
// 真机 1.9MB 日志 BUILD=46887ef74f7f（R123 版）仍崩，崩栈末行：
//   IL2CPP.il2cpp_runtime_invoke_Raw <0x7>   ← native P/Invoke
//   IL2CPP.il2cpp_runtime_invoke <0x4b>      ← R123 wrapper（确实生效了）
//   Il2CppSystem.Type:internal_from_handle <0xab>
//   UnityEngine.Events.UnityAction`2:.cctor <0x9f>
//   native fault: ldr w8,[x8,#8] 解引用 method->methodPointer。
//
// 为什么 R123（method==0 守卫）拦不住？反编译实证（不是猜）：
//   System.Type.internal_from_handle 的 method 参数来自静态字段
//     NativeMethodInfoPtr_internal_from_handle_Private_Static_Type_IntPtr_0
//   该字段在 Il2CppSystem.Type..cctor 里用
//     IL2CPP.GetIl2CppMethodByToken(NativeClassPtr, token=100666086) 初始化。
//   GetIl2CppMethodByToken 的反编译：遍历 NativeClassPtr 的方法找 token 匹配，
//   找不到时**不返回 IntPtr.Zero**，而是调
//     Runtime.NativeStructUtils.GetMethodInfoForMissingMethod(token.ToString())
//   生成一个假 MethodInfo 桩：NewMethod() 分配结构，Name=token 字符串，
//   Slot=65535(0xFFFF)，但 methodPointer 字段【未赋值=无效】。
//   这个桩是【非0指针】，所以 R123 的 method==0 守不住；
//   il2cpp_runtime_invoke 解引用 method->methodPointer(+8) -> SIGSEGV。
//
// 判定：not interop 版本错配（Unity 引擎类型的 token 跨 AU 版本稳定，用户已确认
//   InnerSloth 不动底层）。坏桩只在「某 token 在当前 IL2CPP 里解析不到」时出现，
//   属 Il2CppInterop 对缺失方法的表示缺陷。
//
// 修法（真修复·源头·参数语义正确·非绕过）：
//   把 GetMethodInfoForMissingMethod 的方法体改成直接返回 IntPtr.Zero。
//   于是 GetIl2CppMethodByToken 解析不到 token 时返回明确的 null 句柄（而非野指针桩），
//   与「方法不存在」语义一致，R123 的 method==0 守卫这才真正生效：
//   internal_from_handle 拿到 il2cpp_runtime_invoke 返回的 IntPtr.Zero，
//   其 IL 自带 `dup/brtrue → ldnull` 的 null 分支，安全返回 null，不崩。
//   - 只影响「找不到方法」这一异常路径；能解析到的正常方法零变化。
//   - PC 端 interop 现场生成、token 都匹配，不走这里，语义完全不变。
//   - 跨 AU 版本/设备/iOS 通用：不依赖具体 token 值、不做功能跳过。
// 幂等：已是 ldc.i4.0/conv.i/ret 则跳过；原子写回（R78 截断教训）。
class PatchMissingMethodZero
{
    const string METHOD = "GetMethodInfoForMissingMethod";
    static int patched = 0, skipped = 0;

    static AssemblyDefinition Load(string path) => AssemblyDefinition.ReadAssembly(path,
        new ReaderParameters { ReadingMode = ReadingMode.Immediate, InMemory = true,
                               AssemblyResolver = new DefaultAssemblyResolver() });

    // 判定已补丁：方法体恰好 ldc.i4.0 / conv.i / ret（允许中间无 nop）
    static bool IsPatched(MethodDefinition m)
    {
        if (!m.HasBody) return false;
        var ins = m.Body.Instructions;
        var real = ins.Where(i => i.OpCode != OpCodes.Nop).ToArray();
        return real.Length == 3
            && real[0].OpCode == OpCodes.Ldc_I4_0
            && real[1].OpCode == OpCodes.Conv_I
            && real[2].OpCode == OpCodes.Ret;
    }

    static void PatchOne(MethodDefinition m)
    {
        // 期望签名: static IntPtr GetMethodInfoForMissingMethod(string)
        if (m.ReturnType.MetadataType != MetadataType.IntPtr) {
            Console.WriteLine("[r124a] !! 返回类型非 IntPtr，跳过: " + m.FullName);
            skipped++; return;
        }
        if (m.Parameters.Count != 1) {
            Console.WriteLine("[r124a] !! 参数数量非 1，跳过: " + m.FullName);
            skipped++; return;
        }

        var body = new MethodBody(m) { InitLocals = false };
        m.Body = body;
        var il = body.GetILProcessor();
        il.Emit(OpCodes.Ldc_I4_0);   // 0
        il.Emit(OpCodes.Conv_I);     // -> native int (IntPtr.Zero)
        il.Emit(OpCodes.Ret);
        patched++;
        Console.WriteLine("[r124a] " + m.FullName + " -> 返回 IntPtr.Zero（缺失方法不再用野指针桩）");
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_missing_method_zero <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = argv[0];
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            // 精确找 Runtime.NativeStructUtils::GetMethodInfoForMissingMethod
            var targets = mod.GetTypes().SelectMany(t => t.Methods)
                .Where(mm => mm.Name == METHOD && mm.IsStatic && mm.HasBody
                          && mm.ReturnType.MetadataType == MetadataType.IntPtr
                          && mm.Parameters.Count == 1
                          && mm.Parameters[0].ParameterType.MetadataType == MetadataType.String)
                .ToList();
            if (targets.Count == 0) throw new Exception("找不到 " + METHOD + "(string)->IntPtr");
            Console.WriteLine("[r124a] 找到 " + targets.Count + " 个 " + METHOD);

            bool allPatched = targets.All(IsPatched);
            if (allPatched) {
                Console.WriteLine("[r124a] 已全部注入，跳过");
                asm.Dispose();
                return 0;
            }
            foreach (var t in targets) {
                if (IsPatched(t)) { skipped++; Console.WriteLine("[r124a]   已补丁，跳过: " + t.FullName); }
                else PatchOne(t);
            }

            string tmp = path + ".r124atmp";
            asm.Write(tmp);
            asm.Dispose();
            File.Copy(tmp, path, true);
            File.Delete(tmp);
            Console.WriteLine("[r124a] 完成，改写 " + patched + " 个方法，跳过 " + skipped + " 个");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("[r124a] !! 补丁失败: " + e); return 1; }
    }
}
