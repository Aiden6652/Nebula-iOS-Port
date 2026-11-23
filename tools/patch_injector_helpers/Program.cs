using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R116] InjectorHelpers 模块查找兼容性补丁（Mono.Cecil，幂等，可复现）。
//
// 真机日志（immersion-audio-bridge）实锤：
//   TypeInitializationException: Il2CppInterop.Runtime.Injection.InjectorHelpers
//    ---> InvalidOperationException: Sequence contains no matching element
//     at System.Linq.Enumerable.Single[ProcessModule](...)
//     at Il2CppInterop.Runtime.Injection.InjectorHelpers..cctor()
// 根因：InjectorHelpers..cctor 里
//   Il2CppModule = Process.GetCurrentProcess().Modules.Single(x =>
//       x.ModuleName is "GameAssembly.dll" or "GameAssembly.dylib" or "GameAssembly.so" or "UserAssembly.dll"
//       || x.ModuleName.Equals("GameAssembly.dll", OrdinalIgnoreCase));
//   iOS 上 IL2CPP 代码在 UnityFramework（Mach-O framework），模块名既不是 GameAssembly.dll
//   也不是 GameAssembly.dylib，Single 抛异常 -> 整个 InjectorHelpers 类型初始化失败 ->
//   所有 ClassInjector.RegisterTypeInIl2Cpp<T>() 崩 -> 任何用动态类型注入的第三方插件加载失败。
//
// 修法（最小侵入）：把 lambda 里那个 iOS 上永远命不中的死字符串 "GameAssembly.dylib"
//   替换成真实模块名 "UnityFramework"。原 switch 的其它 case 和 Equals 兜底全部保留，
//   不影响 Windows/Android（那边 GameAssembly.dll/.so 仍在，先命中）。
//   幂等：已是 UnityFramework 则跳过；找不到锚点则非零退出、拒绝产出（不静默）。
//
// 写回策略：ReadingMode.Immediate + InMemory，写临时文件再原子替换（R78 曾把 dll 截成 0 字节）。
class PatchInjectorHelpers
{
    const string DEAD = "GameAssembly.dylib";
    const string LIVE = "UnityFramework";

    static AssemblyDefinition Load(string path)
    {
        return AssemblyDefinition.ReadAssembly(path, new ReaderParameters
        {
            ReadingMode = ReadingMode.Immediate,
            InMemory = true,
            AssemblyResolver = new DefaultAssemblyResolver()
        });
    }

    static void Save(AssemblyDefinition asm, string path)
    {
        string tmp = path + ".ihpatch.tmp";
        asm.Write(tmp);
        asm.Dispose();
        File.Copy(tmp, path, true);
        File.Delete(tmp);
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1)
        {
            Console.Error.WriteLine("usage: patch_injector_helpers <Il2CppInterop.Runtime.dll>");
            return 2;
        }
        string path = argv[0];
        if (!File.Exists(path))
        {
            Console.Error.WriteLine("[r116] 文件不存在: " + path);
            return 2;
        }
        try
        {
            var asm = Load(path);
            int deadHits = 0, liveHits = 0;
            foreach (var t in asm.MainModule.GetTypes())
            {
                foreach (var m in t.Methods)
                {
                    if (!m.HasBody) continue;
                    foreach (var ins in m.Body.Instructions)
                    {
                        if (ins.OpCode != OpCodes.Ldstr) continue;
                        var s = ins.Operand as string;
                        if (s == DEAD) { ins.Operand = LIVE; deadHits++; }
                        else if (s == LIVE) { liveHits++; }
                    }
                }
            }
            if (deadHits == 0 && liveHits > 0)
            {
                asm.Dispose();
                Console.WriteLine("[r116] 已幂等：Il2CppInterop.Runtime 模块匹配已是 UnityFramework（命中 " + liveHits + "），跳过");
                return 0;
            }
            if (deadHits == 0)
            {
                asm.Dispose();
                Console.Error.WriteLine("[r116] !! 未找到 " + DEAD + " 字符串锚点（Il2CppInterop 版本变化？），拒绝产出");
                return 3;
            }
            Save(asm, path);
            Console.WriteLine("[r116] Il2CppInterop.Runtime: \"" + DEAD + "\" -> \"" + LIVE + "\"（命中 " + deadHits + " 处）—— iOS 模块名匹配修复");
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("[r116] !! 补丁失败: " + e);
            return 1;
        }
    }
}
