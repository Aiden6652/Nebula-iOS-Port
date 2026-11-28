// R139 (0 降级真修): 根治 Execute 阶段 RegisterTypeInIl2Cpp 注入拿野 class 崩溃。
//   真机 R138 日志（BUILD=b0098ba2d59f）：
//     ✅ Setup 五 hook 全跑完 / PatchAll seq=160 ok=158 / R134-CLOSED NONZERO / XREF 无 Single/ElementAt 崩
//     ❌ 仍两次 native 崩，均在 Execute 阶段（nebula_r99b_run_execute → 反射 InternalInvoke）：
//        1) SIGBUS  @UnityFramework，紧跟 Class_FromName_Hook cctor-init；
//        2) SIGSEGV @mono_class_instance_size(=il2cpp_class_value_size)，反射建对象拿野 class。
//        AudioBridge 报 (被 catch): Class System.Object does not inherit from a class registered in il2cpp。
//
//   根因（IL 反编译 + 真机双证，非猜测）：
//     Il2CppClassPointerStore<T>.cctor 对【非 enum】类的路径是：
//       IL_0019 get_TypeHandle → IL_0025 RuntimeHelpers::RunClassConstructor(T) → IL_0030 br IL_010e(尾部)
//     它【跳过】了 IL_0035 起的 GetIl2CppClass(asm,ns,name) → stsfld NativeClassPtr 直取分支，
//     寄希望于 T.cctor 被 RunClassConstructor 触发去设置 NativeClassPtr。
//     但 R119/R130 把 RunClassConstructor icall DEFER 成 no-op → T.cctor 不跑 → NativeClassPtr=0。
//     于是 RegisterTypeInIl2Cpp<Il2CppToMonoDelegateReference> / <BridgeRunner> 注入时：
//       基类 System.Object / 注入类自身 NativeClassPtr=0 → il2cpp_class_value_size(0) → SIGSEGV。
//     enum 分支（IL_0017 brtrue → IL_0035）本来就走直取，R132 已验证 LoadSceneMode enum 能拿 NONZERO。
//
//   真修（真修复·0 降级·跨 AU 版本/设备/iOS）；唯一最小改动点 = 一条 IL 跳转：
//     把非 enum 分支 IL_0030 的 `br IL_010e`（跳过直取）改成 `br IL_0035`（走 enum 分支
//     那条已验证的 GetIl2CppClass → stsfld NativeClassPtr 直取）。
//     → DEFER 掐断 RunClassConstructor 时 NativeClassPtr 仍由纯 native GetIl2CppClass 正确设置；
//       直取只解析名字+stsfld，不执行目标 cctor，避开 R128/R129 的 MakeGenericType/get_TypeHandle 坏链；
//       DEFER 白名单一个不动，PatchAll 照常 seq=160，0 降级。
//     PC/Win：RunClassConstructor 未被 DEFER（T.cctor 正常跑），此补丁仅多一次幂等 GetIl2CppClass，零行为变化。
//   幂等、命中校验（opcode/operand/目标偏移全核）、原子写回，任何不符拒绝产出。
using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    const string CPS_TYPE = "Il2CppInterop.Runtime.Il2CppClassPointerStore`1";

    static Instruction At(System.Collections.Generic.IList<Instruction> il, int off)
    {
        return il.FirstOrDefault(x => x.Offset == off);
    }

    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("用法: patch_cps_direct <Il2CppInterop.Runtime.dll>");
            return 2;
        }
        string dll = args[0];
        if (!File.Exists(dll)) { Console.Error.WriteLine("[R139] x DLL 不存在: " + dll); return 2; }

        ModuleDefinition mod;
        try { mod = ModuleDefinition.ReadModule(dll, new ReaderParameters { InMemory = true, ReadWrite = true }); }
        catch (Exception e) { Console.Error.WriteLine("[R139] x 读取失败: " + e.Message); return 2; }

        var t = mod.GetTypes().FirstOrDefault(x => x.FullName == CPS_TYPE);
        if (t == null) { Console.Error.WriteLine("[R139] x 找不到类型 " + CPS_TYPE); return 3; }
        var cctor = t.Methods.FirstOrDefault(m => m.Name == ".cctor");
        if (cctor == null || !cctor.HasBody) { Console.Error.WriteLine("[R139] x 找不到 .cctor 或无条件体"); return 3; }

        var il = cctor.Body.Instructions;
        var iLdtoken     = At(il, 0);
        var iGetHandle   = At(il, 5);
        var iStloc0      = At(il, 10);
        var iIsEnum      = At(il, 12);
        var iBrtrue      = At(il, 17);
        var iGetTypeHnd  = At(il, 20);
        var iRunCC       = At(il, 25);
        var iBr          = At(il, 30);
        var iDirectEntry = At(il, 35);   // IL_0023: ldloc.0  (直取分支入口)
        var iGetClass    = At(il, 260);  // IL2CPP::GetIl2CppClass
        var iStsfld      = At(il, 265);  // stsfld NativeClassPtr
        var iTail        = At(il, 270);  // IL_010e: ldloc.0 (公共尾)

        if (new[] { iLdtoken, iGetHandle, iStloc0, iIsEnum, iBrtrue, iGetTypeHnd,
                    iRunCC, iBr, iDirectEntry, iGetClass, iStsfld, iTail }.Any(x => x == null))
        {
            Console.Error.WriteLine("[R139] x IL 指令缺失，拒绝修改（DLL 版本不符？）");
            return 4;
        }
        if (iLdtoken.OpCode.Code != Code.Ldtoken ||
            iIsEnum.OpCode.Code != Code.Callvirt ||
            iRunCC.OpCode.Code != Code.Call ||
            iBr.OpCode.Code != Code.Br ||
            iGetClass.OpCode.Code != Code.Call ||
            iStsfld.OpCode.Code != Code.Stsfld ||
            iDirectEntry.OpCode.Code != Code.Ldloc_0 ||
            iTail.OpCode.Code != Code.Ldloc_0)
        {
            Console.Error.WriteLine("[R139] x IL opcode 不符，拒绝修改");
            return 4;
        }
        var runMref = iRunCC.Operand as MethodReference;
        if (runMref == null || !runMref.FullName.Contains("RuntimeHelpers::RunClassConstructor"))
        {
            Console.Error.WriteLine("[R139] x offset25 非 RunClassConstructor: " + (runMref == null ? "?" : runMref.FullName));
            return 4;
        }
        var getMref = iGetClass.Operand as MethodReference;
        if (getMref == null || !getMref.FullName.Contains("GetIl2CppClass"))
        {
            Console.Error.WriteLine("[R139] x offset260 非 GetIl2CppClass: " + (getMref == null ? "?" : getMref.FullName));
            return 4;
        }

        // 幂等：offset30 的 br 已指向直取入口(35)？
        var curTarget = iBr.Operand as Instruction;
        if (curTarget == iDirectEntry || (curTarget != null && curTarget.Offset == 35))
        {
            Console.WriteLine("[R139] = R139 直取补丁已存在（offset30 br→IL_0023），跳过（幂等）");
            return 0;
        }
        // 原目标必须是尾部 270
        if (curTarget == null || curTarget.Offset != 270)
        {
            Console.Error.WriteLine("[R139] x offset30 br 原目标非 IL_010e(270): " + (curTarget == null ? "null" : curTarget.Offset.ToString()));
            return 4;
        }

        // 唯一改动：br IL_010e → br IL_0023
        iBr.Operand = iDirectEntry;

        string tmp = dll + ".r139.tmp";
        try
        {
            mod.Write(tmp);
            mod.Dispose();
            File.Copy(tmp, dll, true);
            File.Delete(tmp);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("[R139] x 写回失败: " + e.Message);
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
            return 5;
        }
        Console.WriteLine("[R139] OK Il2CppClassPointerStore`1.cctor 非 enum 分支: offset30 br IL_010e → IL_0023 (GetIl2CppClass 直取)");
        Console.WriteLine("[R139]    效果: DEFER 掐断 RunClassConstructor 时 NativeClassPtr 仍由纯 native GetIl2CppClass 设置；直取不执行目标 cctor，避开 R128/R129 泛型坏链；DEFER 白名单不动，PatchAll seq=160，0 降级。");
        return 0;
    }
}
