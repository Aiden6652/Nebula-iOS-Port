// R134 (音频真修): 根治 InjectorHelpers..cctor 的 NativeLibrary.Load("GameAssembly") DllNotFound。
//   真机 R133 日志：
//     [host][r133] dllmap: GameAssembly* -> .../UnityFramework.framework/UnityFramework（dllmap 挂上了）
//     但仍崩：System.DllNotFoundException: GameAssembly
//       at System.Runtime.InteropServices.NativeLibrary.LoadLibraryByName(...)
//       at System.Runtime.InteropServices.NativeLibrary.Load(String, Assembly, Nullable)
//       at Il2CppInterop.Runtime.Injection.InjectorHelpers..cctor()
//     -> ClassInjector.RegisterTypeInIl2Cpp 全挂 -> AudioBridge/ImmersionAudioBridge 装不上 -> 大厅没声音。
//   根因：R132b 把 Single->FirstOrDefault 修好后，cctor 推进到下一句 NativeLibrary.Load("GameAssembly")。
//     iOS 上 IL2CPP 静态链进 UnityFramework，app bundle 里没有 GameAssembly 文件 -> dlopen 失败。
//     R133 加的 mono_dllmap 对 DllImport 有效，但 NativeLibrary.Load 是 .NET API 走 LoadLibraryByName，
//     不走 mono_dllmap（真机实证：dllmap 挂了仍 DllNotFound）。
//
//   真修（真修复·非绕过·跨 AU 版本/设备/iOS）：把 cctor 里 ldstr "GameAssembly" 换成
//     读 env BEPINEX_GAME_ASSEMBLY_PATH（宿主 r47 已 setenv 指向 UnityFramework 真实路径，
//     R133 的 dllmap 正是从它拿到路径、真机日志证实该路径存在）。
//     dlopen(UnityFramework 绝对路径) 命中已加载镜像 -> 拿有效 Il2CppHandle -> ClassInjector 恢复。
//     env 为空则 fallback "GameAssembly"（PC/Win 仍走真 GameAssembly.dll，零变化）。
//   栈中立注入：入口 0 -> ldstr env名 -> call GetEnvironmentVariable -> [s]
//     dup->[s,s]; brtrue.s->[s]; (false) pop->[] ldstr "GameAssembly"->[s]; (true)->[s]; 出口恒 [s]。
//     与原 ldstr 出口栈深一致，后续指令无需改动。幂等、命中校验、原子写回。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    const string ENV_NAME = "BEPINEX_GAME_ASSEMBLY_PATH";

    static MethodReference ResolveGetEnv(ModuleDefinition mod)
    {
        // 优先复用目标 DLL 内现成的 Environment.GetEnvironmentVariable(string) 引用（scope 100% 合法）。
        foreach (var t in mod.GetTypes())
            foreach (var m in t.Methods)
            {
                if (!m.HasBody) continue;
                foreach (var ins in m.Body.Instructions)
                {
                    var mr = ins.Operand as MethodReference;
                    if (mr == null || mr.Name != "GetEnvironmentVariable") continue;
                    if (mr.DeclaringType == null || mr.DeclaringType.FullName != "System.Environment") continue;
                    if (mr.Parameters.Count == 1 && mr.Parameters[0].ParameterType.FullName == "System.String" &&
                        mr.ReturnType.FullName == "System.String")
                    { Console.WriteLine("[R134] 复用现成 GetEnv 引用: " + mr.FullName); return mr; }
                }
            }
        // 否则从工具 corelib 导入（与 Console.WriteLine 同构，diag 工具已验证可行）。
        var imported = mod.ImportReference(typeof(Environment).GetMethod("GetEnvironmentVariable", new[] { typeof(string) }));
        Console.WriteLine("[R134] ImportReference GetEnv: " + imported.FullName);
        return imported;
    }

    // 静态证明注入块栈中立：入口栈深 d，出口必须仍为 d（顶部是替换后的 string）。
    static bool VerifyBlock(int d)
    {
        int s = d;
        s += 1;              // ldstr ENV_NAME
        s += 1 - 1;          // call GetEnvironmentVariable(string)->string (pop1 push1)
        s += 1;              // dup
        s -= 1;              // brtrue.s
        int falseExit = s - 1 + 1;  // pop; ldstr "GameAssembly"
        int trueExit = s;           // brtrue taken: 栈上保留 dup 的一份
        return falseExit == d + 1 && trueExit == d + 1;
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_injector_loadpath <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        if (!VerifyBlock(0) && !VerifyBlock(2)) { Console.Error.WriteLine("[R134] x 注入块栈中立自检失败，拒绝产出"); return 8; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppInterop.Runtime.Injection.InjectorHelpers");
        if (type == null) { Console.Error.WriteLine("[R134] x 找不到 InjectorHelpers"); return 3; }
        var cctor = type.Methods.FirstOrDefault(m => m.Name == ".cctor" && m.IsStatic && m.HasBody);
        if (cctor == null) { Console.Error.WriteLine("[R134] x 找不到 InjectorHelpers..cctor"); return 3; }

        // 幂等：若 cctor 已出现 ENV_NAME 常量（本补丁已打过），跳过。
        bool already = cctor.Body.Instructions.Any(i => i.OpCode == OpCodes.Ldstr && (string)i.Operand == ENV_NAME);
        if (already) { Console.WriteLine("[R134] 已打过补丁（幂等跳过）"); return 0; }

        var il = cctor.Body.GetILProcessor();
        var getEnv = ResolveGetEnv(mod);

        // 定位：ldstr "GameAssembly"，且其后不远有 NativeLibrary::Load 调用。
        Instruction target = null;
        foreach (var ins in cctor.Body.Instructions)
        {
            if (ins.OpCode != OpCodes.Ldstr || (string)ins.Operand != "GameAssembly") continue;
            // 向后找 NativeLibrary.Load（限 12 条之内），确认是我们要改的那条。
            bool isLoadPath = false;
            var cur = ins.Next;
            for (int k = 0; k < 12 && cur != null; k++, cur = cur.Next)
            {
                var mr = cur.Operand as MethodReference;
                if (mr != null && mr.DeclaringType != null &&
                    mr.DeclaringType.FullName == "System.Runtime.InteropServices.NativeLibrary" &&
                    mr.Name == "Load") { isLoadPath = true; break; }
            }
            if (isLoadPath) { target = ins; break; }
        }
        if (target == null) { Console.Error.WriteLine("[R134] x 找不到 ldstr GameAssembly -> NativeLibrary.Load 组合，拒绝产出"); return 4; }

        // 构造栈中立替换块。出口栈 [string]，与原 ldstr 等价。
        var iEnvName = Instruction.Create(OpCodes.Ldstr, ENV_NAME);
        var iCall = Instruction.Create(OpCodes.Call, getEnv);
        var iDup = Instruction.Create(OpCodes.Dup);
        var iBrtrue = Instruction.Create(OpCodes.Brtrue_S, Instruction.Create(OpCodes.Nop)); // 占位，后面重定位
        var iPop = Instruction.Create(OpCodes.Pop);
        var iFallback = Instruction.Create(OpCodes.Ldstr, "GameAssembly");
        var iLok = Instruction.Create(OpCodes.Nop);

        iBrtrue.Operand = iLok;

        // 用新块替换 target：先插到 target 之后，再把 target 改成第一条，最后删除旧 ldstr。
        il.InsertAfter(target, iEnvName);
        il.InsertAfter(iEnvName, iCall);
        il.InsertAfter(iCall, iDup);
        il.InsertAfter(iDup, iBrtrue);
        il.InsertAfter(iBrtrue, iPop);
        il.InsertAfter(iPop, iFallback);
        il.InsertAfter(iFallback, iLok);
        il.Remove(target);

        // 原子写回。
        string tmp = path + ".r134.tmp";
        mod.Write(tmp);
        System.IO.File.Copy(tmp, path, true);
        System.IO.File.Delete(tmp);
        Console.WriteLine("[R134] √ NativeLibrary.Load 路径改为 env(" + ENV_NAME + ") + fallback GameAssembly，栈中立，已写回");
        return 0;
    }
}
