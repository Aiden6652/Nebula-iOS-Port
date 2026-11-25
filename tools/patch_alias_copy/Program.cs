using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R120] 修复 MonoMod ARM64 detour 写 RX 页 SIGSEGV（确定性根治）。
//
// 崩栈（真机 nebula_host.txt 710kb）：
//   Harmony.PatchAll -> ... -> ILHook:Apply -> Context:Refresh ->
//   Detour:Dispose -> Undo -> _RefreshChain -> NativeDetour:Undo ->
//   DetourNativeARMPlatform:Copy -> SIGSEGV @ 写 JIT 页原 VA 0x1474a1638
//
// 根因：R63 dual-map 给 JIT 代码页建了 RW 别名（nebula_jit_alias），
//       入库的 MonoMod.RuntimeDetour.dll 也已注入 nebula_jit_* P/Invoke，
//       但 DetourNativeARMPlatform.Copy/Apply 仍【直接写原 VA】（RX 页）-> SIGSEGV。
//       原页为执行安全(R62-4 教训)保持 RX，绝不能 whole-page 翻 RW。
//
// 修法（本工具）：在 Copy/Apply 入口把写目标经 nebula_jit_alias() 转成 RW 别名：
//   Copy(IntPtr src, IntPtr dst, byte type):  dst = nebula_jit_alias(dst);
//   Apply(NativeDetourData detour):           detour.Method = nebula_jit_alias(detour.Method);
//   nebula_jit_alias 幂等：非 JIT 页原样返回，JIT 页返回同物理 RW 别名。
//   Apply 内所有 detour.Method.Write(...) 随之写别名；Copy 的 dst 亦然。
//   两者都是写端修复，不改 detour 语义、不改读端、不影响正常页。
//
// 幂等：若方法体已含 nebula_jit_alias 调用则跳过。原子写回（R78 截断教训）。
class PatchAliasCopy
{
    const string ARM_TYPE = "MonoMod.RuntimeDetour.Platforms.DetourNativeARMPlatform";
    const string NDD_TYPE = "MonoMod.RuntimeDetour.NativeDetourData";
    const string ALIAS = "nebula_jit_alias";
    static int changed = 0;

    static AssemblyDefinition Load(string path) => AssemblyDefinition.ReadAssembly(path,
        new ReaderParameters { ReadingMode = ReadingMode.Immediate, InMemory = true,
                               AssemblyResolver = new DefaultAssemblyResolver() });

    static void Save(AssemblyDefinition asm, string path)
    {
        string tmp = path + ".r120tmp";
        asm.Write(tmp);
        asm.Dispose();
        File.Copy(tmp, path, true);
        File.Delete(tmp);
    }

    static TypeDefinition FindType(ModuleDefinition mod, string fullName)
        => mod.GetTypes().FirstOrDefault(t => t.FullName == fullName);

    static bool HasAliasCall(MethodDefinition m, MethodReference alias)
        => m.HasBody && m.Body.Instructions.Any(i => i.Operand is MethodReference r && r.Name == ALIAS);

    // Copy(IntPtr src, IntPtr dst, byte type): 入口 dst = nebula_jit_alias(dst)
    static void PatchCopy(MethodDefinition m, MethodReference alias, ILProcessor il)
    {
        if (HasAliasCall(m, alias)) { Console.WriteLine("[r120][Copy] 已注入，跳过"); return; }
        var dst = m.Parameters[1]; // IntPtr dst
        var first = m.Body.Instructions[0];
        il.InsertBefore(first, il.Create(OpCodes.Ldarg, dst));
        il.InsertBefore(first, il.Create(OpCodes.Call, alias));
        il.InsertBefore(first, il.Create(OpCodes.Starg, dst));
        Console.WriteLine("[r120][Copy] 入口插入 dst=nebula_jit_alias(dst)");
        changed++;
    }

    // Apply(NativeDetourData detour): 入口 detour.Method = nebula_jit_alias(detour.Method)
    // detour 是 struct 值参（arg0），用 ldarga 取地址后 ldfld/stfld 改字段。
    static void PatchApply(MethodDefinition m, MethodReference alias, FieldReference methodField, ILProcessor il)
    {
        if (HasAliasCall(m, alias)) { Console.WriteLine("[r120][Apply] 已注入，跳过"); return; }
        var detour = m.Parameters[0];
        var first = m.Body.Instructions[0];
        // 栈: [&detour] 留给 stfld 的对象地址
        il.InsertBefore(first, il.Create(OpCodes.Ldarga, detour));
        // 栈: [&detour, &detour] -> ldfld -> [&detour, method] -> call -> [&detour, alias]
        il.InsertBefore(first, il.Create(OpCodes.Ldarga, detour));
        il.InsertBefore(first, il.Create(OpCodes.Ldfld, methodField));
        il.InsertBefore(first, il.Create(OpCodes.Call, alias));
        il.InsertBefore(first, il.Create(OpCodes.Stfld, methodField));
        Console.WriteLine("[r120][Apply] 入口插入 detour.Method=nebula_jit_alias(detour.Method)");
        changed++;
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_alias_copy <MonoMod.RuntimeDetour.dll>"); return 2; }
        string path = argv[0];
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            var t = FindType(mod, ARM_TYPE);
            if (t == null) throw new Exception("找不到 " + ARM_TYPE);
            var aliasM = t.Methods.FirstOrDefault(x => x.Name == ALIAS && x.IsStatic
                && x.Parameters.Count == 1 && x.ReturnType.FullName == "System.IntPtr");
            if (aliasM == null) throw new Exception("找不到 nebula_jit_alias(IntPtr) 声明（DLL 未注入 R62 shim?）");
            MethodReference alias = mod.ImportReference(aliasM);

            var ndd = FindType(mod, NDD_TYPE);
            if (ndd == null) throw new Exception("找不到 " + NDD_TYPE);
            var mf = ndd.Fields.FirstOrDefault(f => f.Name == "Method");
            if (mf == null) throw new Exception("NativeDetourData 找不到 Method 字段");
            FieldReference methodField = mod.ImportReference(mf);

            var copy = t.Methods.FirstOrDefault(x => x.Name == "Copy" && x.Parameters.Count == 3);
            var apply = t.Methods.FirstOrDefault(x => x.Name == "Apply" && x.Parameters.Count == 1);
            if (copy == null || apply == null || !copy.HasBody || !apply.HasBody)
                throw new Exception("找不到 Copy/Apply 方法体");

            PatchCopy(copy, alias, copy.Body.GetILProcessor());
            PatchApply(apply, alias, methodField, apply.Body.GetILProcessor());

            Save(asm, path);
            Console.WriteLine("[r120] 完成，修改 " + changed + " 处");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("[r120] !! 补丁失败: " + e); return 1; }
    }
}
