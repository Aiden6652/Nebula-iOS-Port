using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [U001] 通用加载器入口补丁（Mono.Cecil，幂等）。
//
// 背景：宿主 nebula_r56_invoke_manual_execute 写死找 IL2CPPChainloader.NebulaManualExecute()
//   —— 这是 NOS 私有方法。虽然它内部其实就是 PreloadInteropAssemblies()+Execute()（通用逻辑），
//   但方法名本身把宿主绑死在 NOS 上。
//
// 修法：在 IL2CPPChainloader 里新增 public static void UniversalExecute()，
//   方法体逐条复制 NebulaManualExecute（含异常处理），宿主改为优先找 UniversalExecute、
//   找不到再回退 NebulaManualExecute。这样同一份宿主既能在通用分支加载任意 BepInEx6 插件，
//   也能在老 main 上回退 NOS 流程。
//
// 幂等：若 UniversalExecute 已存在则跳过。
// 原子写回（R78 截 0 字节教训）。
class PatchUniversalExecute
{
    const string SRC = "NebulaManualExecute";
    const string DST = "UniversalExecute";

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
        string tmp = path + ".uepatch.tmp";
        asm.Write(tmp);
        asm.Dispose();
        File.Copy(tmp, path, true);
        File.Delete(tmp);
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_universal_execute <BepInEx.Unity.IL2CPP.dll>"); return 2; }
        string path = argv[0];
        if (!File.Exists(path)) { Console.Error.WriteLine("[U001] 文件不存在: " + path); return 2; }
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            var t = mod.GetTypes().FirstOrDefault(x => x.FullName == "BepInEx.Unity.IL2CPP.IL2CPPChainloader");
            if (t == null) throw new Exception("找不到 IL2CPPChainloader 类型");

            if (t.Methods.Any(m => m.Name == DST))
            {
                asm.Dispose();
                Console.WriteLine("[U001] 已幂等：UniversalExecute 已存在，跳过");
                return 0;
            }
            var src = t.Methods.FirstOrDefault(m => m.Name == SRC && m.IsStatic && m.Parameters.Count == 0);
            if (src == null || !src.HasBody) throw new Exception("找不到 " + SRC + "() 方法体");

            // 新建方法：public static void UniversalExecute()
            var dst = new MethodDefinition(DST,
                MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
                mod.TypeSystem.Void);
            // 逐条复制指令（浅拷贝 Operand；本方法体只有字符串常量/方法调用/异常，安全）
            foreach (var ins in src.Body.Instructions)
                dst.Body.Instructions.Add(ins);
            foreach (var v in src.Body.Variables)
                dst.Body.Variables.Add(v);
            foreach (var eh in src.Body.ExceptionHandlers)
                dst.Body.ExceptionHandlers.Add(eh);
            dst.Body.InitLocals = src.Body.InitLocals;
            // 原方法体里的 ldstr 含 "[r57] NebulaManualExecute..."，通用版改成中性文案
            foreach (var ins in dst.Body.Instructions)
            {
                if (ins.OpCode == OpCodes.Ldstr && ins.Operand is string s)
                    ins.Operand = s.Replace("NebulaManualExecute", "UniversalExecute")
                                   .Replace("NOS / NebulaLoader 应已加载", "BepInEx plugins 应已加载");
            }
            t.Methods.Add(dst);
            Save(asm, path);
            Console.WriteLine("[U001] 已新增 UniversalExecute()（复制 " + SRC + " 方法体，" + dst.Body.Instructions.Count + " 条指令）—— 通用加载入口就位");
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("[U001] !! 补丁失败: " + e);
            return 1;
        }
    }
}
