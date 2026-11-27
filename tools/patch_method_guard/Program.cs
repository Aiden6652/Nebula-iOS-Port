using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R125] native method 指针有效性守卫（真修复，非绕过）。
//
// 真机 BUILD=8b349c49(R124) 仍崩，且崩栈与 R123 一字不差：
//   NebulaPlugin.LoadInternal -> SceneManager:get_sceneLoaded
//   -> Il2CppObjectPool:Get -> UnityAction`2:.cctor
//   -> Il2CppSystem.Type:internal_from_handle -> il2cpp_runtime_invoke
//   -> SIGSEGV；native fault: ldr w8,[x0,#8]（x0=method，method 页不可读）。
//
// 实证：R123(method==0 守卫) 与 R124a(缺失方法返回0) 都进了 R124 产物，但未生效。
//   method 既不是 0，也不是"找不到"的坏桩，而是【非0野指针】——
//   Type..cctor 早期缓存的 internal_from_handle method 在 iOS 上指向失效 native 内存。
//
// 修法（参数有效性守卫·跨 AU 版本/设备/iOS 通用·成功路径零影响）：
//   把 R123 wrapper 升级为：method==0 或 nebula_method_readable(method)==0 时
//   返回 IntPtr.Zero；否则原样透传 4 参数调 _Raw。
//   nebula_method_readable 是宿主导出的 native 函数（mach_vm_region 校验页可读），
//   由 host 的 dllmap 把 "nebula_guard" 映射到主 dylib 自身。
//   有效 method 恒返回 1，行为与 PC 现场生成 interop 完全一致；
//   只在 native 指针已失效、原本必崩的异常路径上安全降级到 internal_from_handle
//   自带的 null 分支（其 IL: dup/brtrue -> ldnull），不会跳过任何正常功能。
//
// 幂等：已注入 P/Invoke 且 wrapper 调用它则跳过。原子写回（R78 截断教训）。
class PatchMethodGuard
{
    const string TYPE = "Il2CppInterop.Runtime.IL2CPP";
    const string WRAP = "il2cpp_runtime_invoke";
    const string RAW = "il2cpp_runtime_invoke_Raw";
    const string GUARD = "nebula_method_readable";
    const string MODULE = "nebula_guard";
    static int patched = 0, skipped = 0;

    static AssemblyDefinition Load(string path) => AssemblyDefinition.ReadAssembly(path,
        new ReaderParameters { ReadingMode = ReadingMode.Immediate, InMemory = true,
                               AssemblyResolver = new DefaultAssemblyResolver() });

    static MethodDefinition EnsureGuard(ModuleDefinition mod, TypeDefinition type)
    {
        var g = type.Methods.FirstOrDefault(x => x.Name == GUARD);
        if (g != null) return g;

        // static extern bool nebula_method_readable(IntPtr method);  DllImport(nebula_guard)
        g = new MethodDefinition(GUARD,
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.PInvokeImpl
                | MethodAttributes.HideBySig,
            mod.TypeSystem.Boolean);
        var p = new ParameterDefinition("method", ParameterAttributes.None, mod.TypeSystem.IntPtr);
        g.Parameters.Add(p);

        var moduleRef = new ModuleReference(MODULE);
        mod.ModuleReferences.Add(moduleRef);
        g.PInvokeInfo = new PInvokeInfo(PInvokeAttributes.CallConvCdecl
                                        | PInvokeAttributes.CharSetAnsi, GUARD, moduleRef);

        type.Methods.Add(g);
        Console.WriteLine("[r125] 注入 P/Invoke " + GUARD + " -> " + MODULE + ":" + GUARD);
        return g;
    }

    static void RewriteWrapper(MethodDefinition w, MethodDefinition raw, MethodDefinition guard)
    {
        var body = new MethodBody(w) { InitLocals = false };
        w.Body = body;
        var il = body.GetILProcessor();

        var retNull = il.Create(OpCodes.Ldc_I4_0);

        // if (method == 0) goto retNull
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Brfalse_S, retNull);

        // if (!nebula_method_readable(method)) goto retNull
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, guard);
        il.Emit(OpCodes.Brfalse_S, retNull);

        // return _Raw(method, obj, param, exc)
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Ldarg_2);
        il.Emit(OpCodes.Ldarg_3);
        il.Emit(OpCodes.Call, raw);
        il.Emit(OpCodes.Ret);

        // retNull: return IntPtr.Zero
        il.Append(retNull);
        il.Emit(OpCodes.Conv_I);
        il.Emit(OpCodes.Ret);
    }

    static bool WrapperUsesGuard(MethodDefinition w)
        => w.HasBody && w.Body.Instructions.Any(i => i.Operand is MethodReference mr && mr.Name == GUARD);

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_method_guard <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = argv[0];
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            var t = mod.GetTypes().FirstOrDefault(x => x.FullName == TYPE);
            if (t == null) throw new Exception("找不到 " + TYPE);

            var raw = t.Methods.FirstOrDefault(x => x.Name == RAW && x.IsPInvokeImpl);
            var wrapper = t.Methods.FirstOrDefault(x => x.Name == WRAP && !x.IsPInvokeImpl && x.HasBody);
            if (raw == null || wrapper == null) throw new Exception("找不到 R123 wrapper/_Raw（本工具需在 R123 之后运行）");

            var guard = t.Methods.FirstOrDefault(x => x.Name == GUARD);
            if (guard != null && WrapperUsesGuard(wrapper)) {
                Console.WriteLine("[r125] 已注入（wrapper 已调用 " + GUARD + "），跳过");
                asm.Dispose();
                return 0;
            }

            guard = EnsureGuard(mod, t);
            RewriteWrapper(wrapper, raw, guard);

            string tmp = path + ".r125tmp";
            asm.Write(tmp);
            asm.Dispose();
            File.Copy(tmp, path, true);
            File.Delete(tmp);
            Console.WriteLine("[r125] 完成：il2cpp_runtime_invoke wrapper 增加 method 可读性守卫（method==0 或页不可读 -> IntPtr.Zero）");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("[r125] !! 补丁失败: " + e); return 1; }
    }
}
