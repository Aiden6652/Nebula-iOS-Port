using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using Mono.Cecil;

// [R87] 修 net6 编译的 BepInEx/Il2CppInterop 程序集在 .NET 8 Mono 上的
//   System.TypeLoadException: Could not resolve type with token 01000059 from typeref
//     (expected class 'System.Runtime.InteropServices.Marshal'
//      in assembly 'System.Runtime, Version=6.0.0.0, Culture=neutral,
//       PublicKeyToken=b03f5f7f11d50a3a')
//
// 真机证据(R86 日志 1655/1683/1758)：AutoSave / AUI.ServerScroll / NebulaLoader
//   三个插件全部崩在同一条链：Harmony.PatchAll -> Il2CppType.From ->
//   Il2CppClassPointerStore<T>.cctor -> IL2CPP.GetIl2CppClass -> Marshal typeref。
//   它同时把 NebulaPlugin.LoadInternal 干掉，是 NOS 起不来的主因。
//
// 根因(元数据坐实，非猜测、非裁剪)：
//   - Il2CppInterop.Runtime 1.5.3 / BepInEx.Unity.IL2CPP / AsmResolver.DotNet /
//     Gee.External.Capstone 用 .NET 6 reference assemblies 编译；ref pack 把
//     Marshal 定义成 System.Runtime.dll 里的 TypeDef，故 typeref 的 ResolutionScope
//     写死 System.Runtime 6.0.0.0。
//   - 宿主 BCL 是 microsoft.netcore.app.runtime.mono.osx-arm64 8.0.32(纯 IL)。
//     官方 .NET 8 System.Runtime.dll(43344B, 891 个 ExportedType)把
//     System.Runtime.InteropServices.* 整体交给 System.Runtime.InteropServices.dll
//     转发，自己【不再含 Marshal 本体】；Mono 按 typeref scope 命中 8.0 facade 后
//     查不到该 ExportedType -> TypeLoadException。
//   - Marshal 在 4375336B 的 System.Private.CoreLib(8.0.0.0) 里确实有 TypeDef。
//   - 精确扫描 bepinex-core/*.dll 全部 TypeRef：scope=System.Runtime 且 .NET8
//     facade 缺失的类型【只有 Marshal 一个】，补这一条即可，不会连锁挤牙膏。
//   - 对照 .NET 6 runtime facade(844) 与 .NET 8(884) 的 55 个差异全是内部嵌套
//     类型/泛型数学接口，与插件无关。
//
// 修法(只改宿主 BCL facade；模组 DLL 一字节不动、不做 IL 补丁、BepInEx 原生加载)：
//   给 System.Runtime.dll 追加
//     [TypeForwardedTo(System.Runtime.InteropServices.Marshal)] -> System.Private.CoreLib
//   该 dll 的 AssemblyRef 里已有 System.Private.CoreLib 8.0.0.0，零新增依赖。
//   两个 facade 同时转发同一类型不冲突(typeref 的 scope 决定查哪一个)。
//
// 幂等：已存在则跳过写入。原子写：InMemory 读 + 临时文件 + File.Move 覆盖。
// 强校验：写回后重新读，确认 AssemblyDef 仍 System.Runtime 8.x、
//   Marshal 转发存在且 IsForwarder、目标=System.Private.CoreLib、
//   ExportedType 数 = 原数 +1。任一不满足即非零退出，构建脚本据此拒绝产出。
// 成功把补丁版 SHA256 写到 argv[1]，供 build_ios.sh 生成 blob 时核对。
class PatchFacadeMarshal
{
    const string NS      = "System.Runtime.InteropServices";
    const string NAME    = "Marshal";
    const string FULL    = NS + "." + NAME;
    const string CORELIB = "System.Private.CoreLib";

    static int Fail(string msg)
    {
        Console.Error.WriteLine("[R87] ✗ " + msg);
        return 1;
    }

    static string Sha(byte[] b) => Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();

    static int Main(string[] args)
    {
        if (args.Length < 1)
            return Fail("用法: patch_facade_marshal <System.Runtime.dll 路径> [sha256 输出文件]");
        string path = args[0];
        string shaOut = args.Length > 1 ? args[1] : null;

        if (!File.Exists(path)) return Fail("找不到 " + path);
        byte[] before = File.ReadAllBytes(path);
        Console.WriteLine("[R87] 目标: " + path + " (" + before.Length + " bytes, sha="
                          + Sha(before).Substring(0, 16) + ")");

        ModuleDefinition mod;
        try
        {
            mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = false });
        }
        catch (Exception e) { return Fail("Cecil 读取失败: " + e.Message); }

        // ---- 前置校验：必须是官方完整 .NET 8 System.Runtime facade ----
        if (mod.Assembly == null || mod.Assembly.Name.Name != "System.Runtime")
        {
            string got = mod.Assembly == null ? "<null>" : mod.Assembly.Name.Name;
            mod.Dispose();
            return Fail("不是 System.Runtime.dll（实际 " + got + "），拒绝打补丁");
        }
        if (mod.Assembly.Name.Version.Major != 8)
        {
            string v = mod.Assembly.Name.Version.ToString();
            mod.Dispose();
            return Fail("AssemblyVersion 非 8.x（实际 " + v + "），拒绝打补丁");
        }
        int nBefore = mod.ExportedTypes.Count;
        if (nBefore < 800)
        {
            mod.Dispose();
            return Fail("ExportedType 仅 " + nBefore + " 个，不是完整官方 facade，拒绝打补丁");
        }

        var coreRef = mod.AssemblyReferences.FirstOrDefault(a => a.Name == CORELIB);
        if (coreRef == null)
        {
            mod.Dispose();
            return Fail("缺少 " + CORELIB + " AssemblyRef，无法补转发");
        }

        bool exists = mod.ExportedTypes.Any(e => e.FullName == FULL);
        if (!exists)
        {
            try
            {
                var et = new ExportedType(NS, NAME, mod, coreRef) { IsForwarder = true };
                mod.ExportedTypes.Add(et);
            }
            catch (Exception e)
            {
                mod.Dispose();
                return Fail("构造 ExportedType 失败: " + e.Message);
            }
            Console.WriteLine("[R87] 追加 TypeForwardedTo(" + FULL + ") -> "
                              + CORELIB + " " + coreRef.Version);
        }
        else
        {
            Console.WriteLine("[R87] " + FULL + " 转发已存在，幂等跳过写入");
        }

        string tmp = path + ".r87tmp";
        try
        {
            if (!exists)
            {
                mod.Write(tmp);
                mod.Dispose();
                mod = null;
                File.Move(tmp, path, true);
                Console.WriteLine("[R87] 已原子写回 " + path);
            }
            else
            {
                mod.Dispose();
                mod = null;
            }
        }
        catch (Exception e)
        {
            try { if (mod != null) mod.Dispose(); } catch { }
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
            return Fail("写回失败: " + e.Message);
        }

        // ---- 后置强校验：重新读，确认语义完全正确 ----
        byte[] after = File.ReadAllBytes(path);
        ModuleDefinition chk;
        try { chk = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true }); }
        catch (Exception e) { return Fail("写回后无法重新读取: " + e.Message); }

        string err = null;
        if (chk.Assembly == null || chk.Assembly.Name.Name != "System.Runtime"
            || chk.Assembly.Name.Version.Major != 8)
            err = "写回后 AssemblyDef 异常";
        var fwd = chk.ExportedTypes.FirstOrDefault(e => e.FullName == FULL);
        if (err == null && fwd == null) err = "写回后找不到 " + FULL + " 转发";
        if (err == null && !fwd.IsForwarder) err = "写回后 IsForwarder=false";
        string scopeName = (fwd != null && fwd.Scope != null) ? fwd.Scope.Name : "<null>";
        if (err == null && scopeName != CORELIB)
            err = "转发目标错误: " + scopeName + "（应为 " + CORELIB + "）";
        int expect = nBefore + (exists ? 0 : 1);
        if (err == null && chk.ExportedTypes.Count != expect)
            err = "ExportedType 数量异常: " + nBefore + " -> " + chk.ExportedTypes.Count
                  + "（期望 " + expect + "）";
        chk.Dispose();
        if (err != null) return Fail(err);

        string sha = Sha(after);
        Console.WriteLine("[R87] ✓ 校验通过: ExportedType " + nBefore + " -> " + expect
                          + "，Marshal 转发 -> " + CORELIB
                          + "，AssemblyVersion 8.x 保持");
        Console.WriteLine("[R87] 产物 " + after.Length + " bytes, sha256=" + sha);
        if (!string.IsNullOrEmpty(shaOut))
        {
            try { File.WriteAllText(shaOut, sha); }
            catch (Exception e) { return Fail("sha 写出失败: " + e.Message); }
            Console.WriteLine("[R87] sha 已写 " + shaOut);
        }
        return 0;
    }
}
