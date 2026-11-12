using Mono.Cecil;
using Mono.Cecil.Cil;
using System;
using System.Linq;
using System.Collections.Generic;

// [R95] 修 NOS 加载失败：Harmony 给方法打补丁时用 Mono.Cecil 生成动态委托类型临时模块，
//   调 MonoMod.Utils.ReflectionHelper.Load(module) -> module.Write() 时，Cecil 的默认
//   resolver 按文件路径搜不到 System.Private.CoreLib（真机上 BCL 是从内存 blob 加载、
//   磁盘无散文件），于是 AssemblyResolutionException -> HarmonyException: IL Compile Error
//   -> 插件 Load 失败（AUI.ServerScroll / NebulaLoader 全炸，NOS 起不来）。
//
// 修法：在 ReflectionHelper.Load(ModuleDefinition) 的 module.Write() 之前，给临时模块设一个
//   能解析核心程序集的自定义 resolver：把 <HOME>/Documents/NebulaLibs 加进搜索目录。
//   <HOME>/Documents/NebulaLibs 是 nebula_mono_host.c 启动时 mkdir 的「星云舰依赖」目录，
//   由 build_ios.sh/package_ios.sh 把 System.Private.CoreLib.dll 等核心 BCL 落盘进去，
//   Cecil 即可解析。
//
// 用法：patch_cecil_resolver <MonoMod.Utils.dll> [out.dll]
//   不指定 out 则原地覆盖；指定则写到新文件（便于先验证再覆盖）。
class P
{
    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: patch_cecil_resolver <MonoMod.Utils.dll> [out]"); return 1; }
        string path = args[0];
        string outp = args.Length > 1 ? args[1] : path;
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("[R95] 找不到 " + path); return 1; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { ReadWrite = false, InMemory = true });

        Console.WriteLine("[R95] AssemblyRefs:");
        foreach (var r in mod.AssemblyReferences) Console.WriteLine("    " + r.Name + " " + r.Version);

        var coreRef = mod.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Private.CoreLib")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Runtime")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "mscorlib")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "netstandard");
        Console.WriteLine("[R95] 核心 ref: " + (coreRef == null ? "<null>" : coreRef.FullName));

        var t = mod.Types.FirstOrDefault(x => x.FullName == "MonoMod.Utils.ReflectionHelper");
        if (t == null) { Console.Error.WriteLine("[R95] 找不到 MonoMod.Utils.ReflectionHelper"); return 1; }
        var m = t.Methods.FirstOrDefault(x => x.Name == "Load" && x.Parameters.Count == 1
            && x.Parameters[0].ParameterType.FullName == "Mono.Cecil.ModuleDefinition");
        if (m == null || !m.HasBody) { Console.Error.WriteLine("[R95] 找不到 Load(ModuleDefinition)"); return 1; }
        Console.WriteLine("[R95] 命中 " + m.FullName);

        // ---- Cecil 引用：工具引用的 bepinex-core/Mono.Cecil.dll 与目标 DLL 引用的是同一份，版本零偏差 ----
        var cecilAsm = mod.AssemblyReferences.First(a => a.Name == "Mono.Cecil");
        var defaultResolverType = new TypeReference("Mono.Cecil", "DefaultAssemblyResolver", mod, cecilAsm);
        var baseResolverType   = new TypeReference("Mono.Cecil", "BaseAssemblyResolver", mod, cecilAsm);
        var modDefType         = new TypeReference("Mono.Cecil", "ModuleDefinition", mod, cecilAsm);
        var iasmType           = new TypeReference("Mono.Cecil", "IAssemblyResolver", mod, cecilAsm);

        var ctor = new MethodReference(".ctor", mod.TypeSystem.Void, defaultResolverType) { HasThis = true };
        var addDir = new MethodReference("AddSearchDirectory", mod.TypeSystem.Void, baseResolverType) { HasThis = true };
        addDir.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));
        var setResolver = new MethodReference("set_AssemblyResolver", mod.TypeSystem.Void, modDefType) { HasThis = true };
        setResolver.Parameters.Add(new ParameterDefinition(iasmType));

        // ---- 核心类型：手动指向目标已有的 coreRef，避免引入错误版本的 mscorlib ----
        var envType = new TypeReference("System", "Environment", mod, coreRef);
        var getEnv = new MethodReference("GetEnvironmentVariable", mod.TypeSystem.String, envType) { HasThis = false };
        getEnv.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));
        var pathType = new TypeReference("System.IO", "Path", mod, coreRef);
        var combine = new MethodReference("Combine", mod.TypeSystem.String, pathType) { HasThis = false };
        combine.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));
        combine.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));
        combine.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));

        // ---- 在 ldarg.0(module) 之后、ldloc.0(stream) 之前插入 ----
        // 栈：ldarg.0 -> [module]
        //   dup                                        [module, module]
        //   newobj DefaultAssemblyResolver              [module, module, resolver]
        //   dup                                        [module, module, resolver, resolver]
        //   dup                                        ... resolver, resolver, resolver
        //   ldstr "NebulaLibs"                          ... resolver, "NebulaLibs"(相对当前目录的兜底)
        //   callvirt AddSearchDirectory                 [module, module, resolver, resolver]
        //   dup                                        ... resolver, resolver, resolver
        //   ldstr "HOME"                                ... resolver, "HOME"
        //   call  GetEnvironmentVariable                ... resolver, home
        //   ldstr "Documents"                           ... resolver, home, "Documents"
        //   ldstr "NebulaLibs"                          ... resolver, home, "Documents", "NebulaLibs"
        //   call  Path.Combine(3)                       ... resolver, combined
        //   callvirt AddSearchDirectory                 [module, module, resolver]
        //   callvirt set_AssemblyResolver               [module]  <- module 保留给后面的 Write(module, stream)
        //
        // 多候选目录，抵御真机目录歧义（成品放 HOME/Documents 还是 HOME/Documents/AmongUs）：
        //   ① 相对 "NebulaLibs"（cwd 为游戏根时命中）
        //   ② HOME/Documents/NebulaLibs（nebula_mono_host.c 启动时 mkdir 的目录）
        // [R95] 幂等：若 ReflectionHelper.Load 已含 "NebulaLibs" 标记，说明已注入，跳过
        if (m.Body.Instructions.Any(i => (i.Operand as string) == "NebulaLibs"))
        {
            Console.WriteLine("[R95] 已检测到注入标记，幂等跳过");
            mod.Write(outp);
            return 0;
        }

        var il = m.Body.GetILProcessor();
        var ldarg0 = m.Body.Instructions.First(i => i.OpCode == OpCodes.Ldarg_0);

        var ins = new List<Instruction>();
        ins.Add(il.Create(OpCodes.Dup));
        ins.Add(il.Create(OpCodes.Newobj, ctor));
        ins.Add(il.Create(OpCodes.Dup));
        // 候选①：相对 NebulaLibs
        ins.Add(il.Create(OpCodes.Dup));
        ins.Add(il.Create(OpCodes.Ldstr, "NebulaLibs"));
        ins.Add(il.Create(OpCodes.Callvirt, addDir));
        // 候选②：HOME/Documents/NebulaLibs
        ins.Add(il.Create(OpCodes.Dup));
        ins.Add(il.Create(OpCodes.Ldstr, "HOME"));
        ins.Add(il.Create(OpCodes.Call, getEnv));
        ins.Add(il.Create(OpCodes.Ldstr, "Documents"));
        ins.Add(il.Create(OpCodes.Ldstr, "NebulaLibs"));
        ins.Add(il.Create(OpCodes.Call, combine));
        ins.Add(il.Create(OpCodes.Callvirt, addDir));
        ins.Add(il.Create(OpCodes.Callvirt, setResolver));

        Instruction anchor = ldarg0;
        foreach (var i in ins) { il.InsertAfter(anchor, i); anchor = i; }

        Console.WriteLine("[R95] 已插入 " + ins.Count + " 条 IL（module.Write 前设 AssemblyResolver）");

        mod.Write(outp);
        Console.WriteLine("[R95] 已写入 " + outp);
        return 0;
    }
}
