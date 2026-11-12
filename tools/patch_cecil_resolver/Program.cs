using Mono.Cecil;
using Mono.Cecil.Cil;
using System;
using System.Linq;
using System.Collections.Generic;

// [R95c] 修 NOS 加载失败（R94 真因 / R95b 复盘）
//
//  R94 真因：Harmony 用 Mono.Cecil 生成动态委托临时模块，MonoMod.Utils.ReflectionHelper.Load
//    调 module.Write(stream) 时，模块自带的 DefaultAssemblyResolver 按文件路径解析不到
//    System.Private.CoreLib（BCL 从内存 blob 加载、默认搜索目录里没有），抛
//    Mono.Cecil.AssemblyResolutionException -> HarmonyException: IL Compile Error -> 插件 Load 失败。
//
//  R95/R95b 的错误：注入 IL 时手工 new MethodReference("set_AssemblyResolver", ...) 挂在
//    Mono.Cecil.ModuleDefinition 上。但 Mono.Cecil 0.11.4 的 ModuleDefinition.AssemblyResolver
//    是【只读属性】（只有 get_AssemblyResolver，没有 setter；set_AssemblyResolver 属于
//    ReaderParameters）。Mono 一 JIT ReflectionHelper.Load 就抛
//    MissingMethodException: void Mono.Cecil.ModuleDefinition.set_AssemblyResolver(...) ，
//    比 R94 更早崩，所有 Harmony 补丁全挂。换 DLL / 查版本冲突都无解——官方本就没这个 setter。
//
//  [R95c] 正确修法：不新建 resolver、不调 setter。
//    取 module 自带的 IAssemblyResolver（真机堆栈实锤是 DefaultAssemblyResolver，
//    继承 BaseAssemblyResolver），cast 后 AddSearchDirectory(<HOME>/Documents/BepInEx/core)。
//    宿主启动时已把 blob 里的全套 BCL（含 System.Private.CoreLib.dll）释放到
//    <HOME>/Documents/BepInEx/core，磁盘上现成就有，Cecil 即可解析。
//    => 不需要任何额外散文件，交付仍是单一 dylib。
//
// 注入点：ReflectionHelper.Load(ModuleDefinition) 的第一个 ldarg.0 之后、ldloc.0 之前。
// 栈平衡：ldarg.0 -> [module] ... 插桩后仍为 [module]，正好留给后面的 Write(module, stream)。
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
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("[R95c] 找不到 " + path); return 1; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { ReadWrite = false, InMemory = true });

        Console.WriteLine("[R95c] AssemblyRefs:");
        foreach (var r in mod.AssemblyReferences) Console.WriteLine("    " + r.Name + " " + r.Version);

        // 核心类型一律指向目标 DLL 已有的 coreRef（这里是 System.Runtime 5.0.0.0 facade，
        // 真机运行时一致），避免引入错误版本的 mscorlib。
        var coreRef = mod.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Private.CoreLib")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "System.Runtime")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "mscorlib")
                   ?? mod.AssemblyReferences.FirstOrDefault(a => a.Name == "netstandard");
        if (coreRef == null) { Console.Error.WriteLine("[R95c] 找不到核心 ref"); return 1; }
        Console.WriteLine("[R95c] 核心 ref: " + coreRef.FullName);

        var t = mod.Types.FirstOrDefault(x => x.FullName == "MonoMod.Utils.ReflectionHelper");
        if (t == null) { Console.Error.WriteLine("[R95c] 找不到 MonoMod.Utils.ReflectionHelper"); return 1; }
        var m = t.Methods.FirstOrDefault(x => x.Name == "Load" && x.Parameters.Count == 1
            && x.Parameters[0].ParameterType.FullName == "Mono.Cecil.ModuleDefinition");
        if (m == null || !m.HasBody) { Console.Error.WriteLine("[R95c] 找不到 Load(ModuleDefinition)"); return 1; }
        Console.WriteLine("[R95c] 命中 " + m.FullName);

        // ---- Cecil 引用：目标 DLL 与工具引用的是同一份 bepinex-core/Mono.Cecil.dll，版本零偏差 ----
        var cecilAsm = mod.AssemblyReferences.First(a => a.Name == "Mono.Cecil");
        var modDefType       = new TypeReference("Mono.Cecil", "ModuleDefinition",     mod, cecilAsm);
        var baseResolverType = new TypeReference("Mono.Cecil", "BaseAssemblyResolver", mod, cecilAsm);
        var iasmType         = new TypeReference("Mono.Cecil", "IAssemblyResolver",    mod, cecilAsm);

        // IAssemblyResolver ModuleDefinition::get_AssemblyResolver()  —— 真实存在（R95c）
        var getResolver = new MethodReference("get_AssemblyResolver", iasmType, modDefType) { HasThis = true };

        // void BaseAssemblyResolver::AddSearchDirectory(string)  —— 真实存在、public（R95c）
        var addDir = new MethodReference("AddSearchDirectory", mod.TypeSystem.Void, baseResolverType) { HasThis = true };
        addDir.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));

        // string System.Environment::GetEnvironmentVariable(string)
        var envType = new TypeReference("System", "Environment", mod, coreRef);
        var getEnv = new MethodReference("GetEnvironmentVariable", mod.TypeSystem.String, envType) { HasThis = false };
        getEnv.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));

        // string System.String::Concat(string, string)  —— 对 null 视为空串，绝不抛异常。
        // 不用 Path.Combine：HOME 万一为空会抛 ArgumentNullException 反而搞挂 Load。
        // Concat(GetEnvironmentVariable("HOME"), "/Documents/BepInEx/core")
        //   → <HOME>/Documents/BepInEx/core（宿主 R80 释放 BCL 的目录）。
        var strType = new TypeReference("System", "String", mod, coreRef);
        var concat2 = new MethodReference("Concat", mod.TypeSystem.String, strType) { HasThis = false };
        concat2.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));
        concat2.Parameters.Add(new ParameterDefinition(mod.TypeSystem.String));

        // 幂等：已含标记则跳过（重复注入会重复加目录，虽无害但没必要）
        const string MARK = "/Documents/BepInEx/core";
        if (m.Body.Instructions.Any(i => (i.Operand as string) == MARK))
        {
            Console.WriteLine("[R95c] 已检测到注入标记，幂等跳过");
            mod.Write(outp);
            return 0;
        }

        // 栈推演（ldarg.0 之后）：
        //   dup                                 [module, module]
        //   callvirt get_AssemblyResolver       [module, IAssemblyResolver]
        //   castclass BaseAssemblyResolver     [module, resolver]
        //   ldstr "HOME"                       [module, resolver, "HOME"]
        //   call  GetEnvironmentVariable       [module, resolver, home]
        //   ldstr "/Documents/BepInEx/core"   [module, resolver, home, rel]
        //   call  String.Concat(2)            [module, resolver, path]
        //   callvirt AddSearchDirectory        [module]   <- 留给后面的 Write(module, stream)
        var il = m.Body.GetILProcessor();
        var ldarg0 = m.Body.Instructions.First(i => i.OpCode == OpCodes.Ldarg_0);

        var ins = new List<Instruction>();
        ins.Add(il.Create(OpCodes.Dup));
        ins.Add(il.Create(OpCodes.Callvirt, getResolver));
        ins.Add(il.Create(OpCodes.Castclass, baseResolverType));
        ins.Add(il.Create(OpCodes.Ldstr, "HOME"));
        ins.Add(il.Create(OpCodes.Call, getEnv));
        ins.Add(il.Create(OpCodes.Ldstr, MARK));
        ins.Add(il.Create(OpCodes.Call, concat2));
        ins.Add(il.Create(OpCodes.Callvirt, addDir));

        Instruction anchor = ldarg0;
        foreach (var i in ins) { il.InsertAfter(anchor, i); anchor = i; }

        Console.WriteLine("[R95c] 已插入 " + ins.Count + " 条 IL（Write 前给自带 resolver 加 BepInEx/core 搜索目录）");
        mod.Write(outp);
        Console.WriteLine("[R95c] 已写入 " + outp);
        return 0;
    }
}
