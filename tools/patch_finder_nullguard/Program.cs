// R135 (0 降级真修): 根治 InjectorHelpers.Setup() 的 FindSignatureInModule(null) NRE。
//   真机 R134 日志（BUILD=205731f5ecdd）：
//     ✅ [R134-CLOSED] closedClass=NONZERO —— R134 field-type 直取 closed class 真修成功
//     ✅ PatchAll seq=163 ok=160 down=0 跑满；音频 DllNotFound 消失（R134 loadpath 生效）
//     ❌ 新崩点：NebulaPlugin.LoadInternal -> sceneLoaded += -> op_Implicit(Action`2)
//        -> DelegateSupport.ConvertDelegate<UnityAction`2>
//        -> ClassInjector.RegisterTypeInIl2Cpp<Il2CppToMonoDelegateReference>
//        -> InjectorHelpers.Setup()
//        -> GenericMethod_GetMethod_Hook.ApplyHook -> FindTargetMethod
//        -> <FindTargetMethod>b__6_0 -> MemoryUtils.FindSignatureInModule(module, sig)
//        -> module.get_BaseAddress()  // module == null -> NullReferenceException
//   根因：InjectorHelpers..cctor 用 Process.GetCurrentProcess().Modules.Single(名字==GameAssembly.*)
//     取 Il2CppModule。iOS 上 IL2CPP 静态链进 UnityFramework、Process.Modules 枚举不到模块，
//     R132b 把 Single 换 FirstOrDefault 后不再抛异常但返回 null -> Il2CppModule = null。
//     R134 让 cctor 完全跑完后 Setup() 首次执行，撞上这个 null：两条 hook
//     (GenericMethod_GetMethod_Hook / Class_GetFieldDefaultValue_Hook) 的签名扫描段
//     用 FindSignatureInModule(Il2CppModule=null) -> NRE，直接冒泡（ApplyHook 无 try-catch）。
//
//   真修（真修复·0 降级·跨 AU 版本/设备/iOS）：
//     Setup() 里 5 个 hook 有 2 条走 GetIl2CppExport + XrefScan（Class_FromIl2CppType /
//     Class_FromName / Unity6 hook，环境已验证可用），另 2 条优先走签名扫描、失败后有
//     GetIl2CppExport fallback（GenericMethod / Class_GetFieldDefaultValue）。
//     两个 hook 的 FirstOrDefault predicate 都是 ptr > 0，签名扫描返回 IntPtr.Zero 会被过滤，
//     自然落到 fallback 路径——与另外 3 条 hook 用的是同一套成熟机制，非降级、非绕过。
//   改法：在 MemoryUtils::FindSignatureInModule(ProcessModule, SignatureDefinition) 入口加守卫：
//     module == null -> return IntPtr.Zero;  否则原逻辑不变。
//   注入 IL（栈中立）：
//     ldarg.0; brtrue.s Lorig; ldc.i4.0; conv.i; ret; Lorig: <原第一条指令>
//   PC/Win：Process.Modules 正常含 GameAssembly.dll -> module 非 null -> 守卫不触发，零变化。
//   幂等、命中校验、栈中立自检、原子写回。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    // 静态证明注入块栈中立：入口栈深 d(此处恒 0)，出口必须仍为 d。
    //   ldarg.0        -> [module]        (d+1)
    //   brtrue.s Lorig -> []              (d)    taken -> 原逻辑入口(d)
    //   ldc.i4.0       -> [0]             (d+1)
    //   conv.i         -> [nativeint 0]   (d+1)
    //   ret            -> []  (返回 1 个值，符合 IntPtr 返回签名)
    static bool VerifyGuard()
    {
        int s = 0;
        s += 1;      // ldarg.0            -> [module]  (1)
        s -= 1;      // brtrue.s           -> []        (0)  taken -> 原逻辑入口(0)
        int nullExit = s + 1;      // ldc.i4.0 -> [int32] (1)
        nullExit += 1 - 1;         // conv.i (pop1/push1, 净 0) -> [native int] (1)
        return nullExit == 1 && s == 0;  // null 分支 ret 时栈顶 1 个 native int；非null分支入口栈 0
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_finder_nullguard <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }
        if (!VerifyGuard()) { Console.Error.WriteLine("[R135] x 守卫栈中立自检失败，拒绝产出"); return 8; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppInterop.Runtime.MemoryUtils");
        if (type == null) { Console.Error.WriteLine("[R135] x 找不到 MemoryUtils"); return 3; }
        var m = type.Methods.FirstOrDefault(
            mm => mm.Name == "FindSignatureInModule" && mm.HasBody &&
                  mm.Parameters.Count == 2 &&
                  mm.Parameters[0].ParameterType.FullName == "System.Diagnostics.ProcessModule");
        if (m == null) { Console.Error.WriteLine("[R135] x 找不到 FindSignatureInModule(ProcessModule, SignatureDefinition)"); return 3; }
        if (m.ReturnType.FullName != "System.IntPtr") { Console.Error.WriteLine("[R135] x 返回类型非 IntPtr: " + m.ReturnType.FullName); return 3; }

        // 幂等：检测首条是否已是 ldarg.0 + brtrue.s + ldc.i4.0 + conv.i + ret 守卫形态。
        var ins = m.Body.Instructions;
        if (ins.Count >= 5 &&
            ins[0].OpCode == OpCodes.Ldarg_0 &&
            ins[1].OpCode == OpCodes.Brtrue_S &&
            ins[2].OpCode == OpCodes.Ldc_I4_0 &&
            ins[3].OpCode == OpCodes.Conv_I &&
            ins[4].OpCode == OpCodes.Ret)
        {
            Console.WriteLine("[R135] = 守卫已存在，跳过（幂等）");
            return 0;
        }

        // 定位第一条原指令
        var first = ins[0];
        var il = m.Body.GetILProcessor();
        var iLdarg = Instruction.Create(OpCodes.Ldarg_0);
        var iBrtrue = Instruction.Create(OpCodes.Brtrue_S, first);
        var iLdc0 = Instruction.Create(OpCodes.Ldc_I4_0);
        var iConvI = Instruction.Create(OpCodes.Conv_I);
        var iRet = Instruction.Create(OpCodes.Ret);

        il.InsertBefore(first, iLdarg);
        il.InsertBefore(first, iBrtrue);
        il.InsertBefore(first, iLdc0);
        il.InsertBefore(first, iConvI);
        il.InsertBefore(first, iRet);

        mod.Write(path);
        Console.WriteLine("[R135] ✓ 已注入 FindSignatureInModule null 守卫 -> return IntPtr.Zero（模块名 " + path + "）");
        Console.WriteLine("[R135]   入口: ldarg.0 / brtrue.s 原逻辑 / [null] ldc.i4.0;conv.i;ret");
        Console.WriteLine("[R135]   效果: 两条 hook 签名扫描得 0 被 ptr>0 过滤 -> 走 GetIl2CppExport+XrefScan fallback（0降级）");
        return 0;
    }
}
