using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R82] 三合一 IL 补丁，全部基于 Mono.Cecil，可复现。
//
// 修三件真机实测的事（崩栈 / 日志实锤，见提交说明）：
//  (1) 0Harmony.dll   HarmonyLib.Internal.RuntimeFixes.StackTraceFixes::Install() -> ret
//      new Harmony() 会触发 Harmony..cctor -> Install()，里面建 3 个 MonoMod
//      Detour/NativeDetour -> FlushICache -> 首次 JIT 编译含 ldstr 的方法 ->
//      mono_ldstr_checked -> mono_gc_alloc_string -> sgen_try_alloc_obj_nolock SIGSEGV。
//      StackTraceFixes 纯粹是"patch 后堆栈跟踪显示更干净"的美化，禁掉零功能损失，
//      且 Harmony.PatchAll()（插件真正打补丁的路径）完全不受影响。
//  (2) MonoMod.RuntimeDetour.dll  DetourNativeARMPlatform..ctor 的
//      ShouldFlushICache: ldc.i4.1 -> ldc.i4.0
//      FlushICache 开头就是 if(!ShouldFlushICache) ret；宿主 make_executable 已用
//      sys_icache_invalidate 刷过（build_mono_ios.sh:509），这层是冗余且正是崩点。
//      禁掉后 detour 照常装（MemAlloc/MakeWritable/MakeExecutable 走 nebula_jit_*）。
//  (3) BepInEx.Core.dll  BaseChainloader`1/<>c::<ModifyLoadOrder>b__27_4 -> ldc.i4.0; ret
//      原逻辑 filter.ProcessName.Replace(".exe","").Equals(Paths.ProcessName) == false
//      -> true(不匹配) -> Enumerable.All 全 true 就跳过插件。iOS 单游戏进程，
//      过滤器本就无意义；恒返回 false(匹配) 让"AmongUs"/"Among Us"及未来任何
//      命名都放行，正是用户要的"带不带空格都认"。
//
// 写回策略：ReadingMode.Immediate + InMemory，写临时文件再原子替换。
// R78 曾因同路径写回把 DLL 截成 0 字节，这里必须绕开。
class PatchR82
{
    static int changed = 0;

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
        string tmp = path + ".r82tmp";
        asm.Write(tmp);
        asm.Dispose();
        File.Copy(tmp, path, true);
        File.Delete(tmp);
    }

    static TypeDefinition FindType(ModuleDefinition mod, string fullName)
    {
        foreach (var t in mod.GetTypes())
            if (t.FullName == fullName) return t;
        return null;
    }

    static MethodDefinition FindMethod(TypeDefinition t, string name)
    {
        return t?.Methods.FirstOrDefault(m => m.Name == name);
    }

    // 把方法体清空成单条/两条指令
    static void ReplaceBody(MethodDefinition m, ILProcessor il, params OpCode[] ops)
    {
        var body = m.Body;
        body.Instructions.Clear();
        body.ExceptionHandlers.Clear();
        body.Variables.Clear();
        body.InitLocals = false;
        foreach (var op in ops) il.Append(il.Create(op));
    }

    static void PatchHarmony(string path)
    {
        var asm = Load(path);
        var mod = asm.MainModule;
        var t = FindType(mod, "HarmonyLib.Internal.RuntimeFixes.StackTraceFixes");
        if (t == null) throw new Exception("0Harmony: 找不到 StackTraceFixes 类型");
        var m = FindMethod(t, "Install");
        if (m == null || !m.HasBody) throw new Exception("0Harmony: 找不到 Install() 方法体");
        int before = m.Body.Instructions.Count;
        var il = m.Body.GetILProcessor();
        ReplaceBody(m, il, OpCodes.Ret);
        Console.WriteLine("[r82][0Harmony] StackTraceFixes::Install() " + before + " 条指令 -> ret（禁用冗余堆栈 detour）");
        Save(asm, path);
        changed++;
    }

    static void PatchDetour(string path)
    {
        var asm = Load(path);
        var mod = asm.MainModule;
        var t = FindType(mod, "MonoMod.RuntimeDetour.Platforms.DetourNativeARMPlatform");
        if (t == null) throw new Exception("MonoMod.RuntimeDetour: 找不到 DetourNativeARMPlatform");
        var ctor = t.Methods.FirstOrDefault(x => x.IsConstructor && !x.IsStatic && x.Parameters.Count == 0);
        if (ctor == null || !ctor.HasBody) throw new Exception("MonoMod.RuntimeDetour: 找不到 .ctor()");
        int hits = 0;
        foreach (var ins in ctor.Body.Instructions)
        {
            // IL_0001: ldc.i4.1 ; IL_0002: stfld ShouldFlushICache
            if (ins.OpCode == OpCodes.Ldc_I4_1)
            {
                var next = ins.Next;
                if (next != null && next.OpCode == OpCodes.Stfld &&
                    next.Operand is FieldReference fr && fr.Name == "ShouldFlushICache")
                {
                    ins.OpCode = OpCodes.Ldc_I4_0;
                    hits++;
                }
            }
        }
        if (hits != 1) throw new Exception("MonoMod.RuntimeDetour: ShouldFlushICache 赋值命中 " + hits + " 处（应为 1）");
        Console.WriteLine("[r82][MonoMod] .ctor: ldc.i4.1 -> ldc.i4.0（ShouldFlushICache=false，禁冗余 FlushICache）");
        Save(asm, path);
        changed++;
    }

    static void PatchFilter(string path)
    {
        var asm = Load(path);
        var mod = asm.MainModule;
        var t = FindType(mod, "BepInEx.Bootstrap.BaseChainloader`1/<>c");
        if (t == null) throw new Exception("BepInEx.Core: 找不到 BaseChainloader`1/<>c");
        var m = t.Methods.FirstOrDefault(x => x.Name.Contains("b__27_4"));
        if (m == null || !m.HasBody) throw new Exception("BepInEx.Core: 找不到 b__27_4");
        int before = m.Body.Instructions.Count;
        var il = m.Body.GetILProcessor();
        // 返回 false = "该过滤器匹配" -> Enumerable.All 为 false -> 不跳过插件
        ReplaceBody(m, il, OpCodes.Ldc_I4_0, OpCodes.Ret);
        Console.WriteLine("[r82][BepInEx.Core] b__27_4 " + before + " 条指令 -> ldc.i4.0; ret（进程过滤器恒放行，带不带空格都认）");
        Save(asm, path);
        changed++;
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1)
        {
            Console.Error.WriteLine("usage: patch_r82 <bepinex-core-dir>");
            return 2;
        }
        string d = argv[0];
        try
        {
            PatchHarmony(Path.Combine(d, "0Harmony.dll"));
            PatchDetour(Path.Combine(d, "MonoMod.RuntimeDetour.dll"));
            PatchFilter(Path.Combine(d, "BepInEx.Core.dll"));
            Console.WriteLine("[r82] 完成，共修改 " + changed + " 个 DLL");
            return changed == 3 ? 0 : 3;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("[r82] !! 补丁失败: " + e);
            return 1;
        }
    }
}
