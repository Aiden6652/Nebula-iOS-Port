using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R122] 修 async 状态机类 cctor 把 IntPtr.Zero 传给 il2cpp_runtime_class_init 的 SIGSEGV。
//
// 真机 1.5MB 日志（universal-loader BUILD=015b0fbee21d）崩栈：
//   Harmony.PatchAll -> ILHook.Apply -> Detour:_RefreshChain ->
//   DetourHelper:GetNativeStart -> DetourRuntimeILPlatform:GetFunctionPointer ->
//   System.RuntimeMethodHandle:GetFunctionPointer -> mono JIT ->
//   mono_runtime_class_init_full -> _CoUseZipline_d__39:.cctor ->
//   IL2CPP:il2cpp_runtime_class_init -> SIGSEGV.
//
// 根因：Il2CppInterop 生成的 interop 类（尤其 C# async 状态机 <UseZipline>d__39，
//   IL2CPP 侧被裁剪/不存在）cctor 里调 il2cpp_runtime_class_init(NativeClassPtr)。
//   IL2CPP.GetIl2CppClass 找不到类时返回 IntPtr.Zero，而 il2cpp_runtime_class_init
//   是裸 P/Invoke、无 null 检查 -> 传 0 进 libil2cpp -> SIGSEGV。
//   R119 的 DEFER 只拦 RunClassConstructor icall，拦不住 JIT 内部自动跑的 cctor。
//
// 修法（真修复，参数有效性守卫，非绕过）：
//   把 IL2CPP.il2cpp_runtime_class_init(IntPtr) P/Invoke 改名为 _Raw 并保留 native
//   EntryPoint；新建同名托管 wrapper：klass==IntPtr.Zero 时 no-op 返回，否则调 _Raw。
//   - 跨程序集调用点（设备 Documents/BepInEx/interop/Assembly-CSharp.dll 里的
//     interop 类 cctor）按名字+签名重解析，自动绑到新 wrapper，零改动。
//   - 语义正确：NativeClassPtr==0 表示 IL2CPP 侧没有该类，无 native cctor 可初始化；
//     状态机 MoveNext 是 managed IL，不依赖 native class init，跳过不影响功能。
//   - 跨版本/设备/iOS：只对 null 参数生效，任何 AU 版本/设备/iOS 版本行为一致。
// 幂等：已有 _Raw 则跳过；原子写回（R78 截断教训）。
class PatchClassInitNullGuard
{
    const string TYPE = "Il2CppInterop.Runtime.IL2CPP";
    const string METHOD = "il2cpp_runtime_class_init";
    const string RAW = METHOD + "_Raw";
    static int patched = 0, skipped = 0;

    static AssemblyDefinition Load(string path) => AssemblyDefinition.ReadAssembly(path,
        new ReaderParameters { ReadingMode = ReadingMode.Immediate, InMemory = true,
                               AssemblyResolver = new DefaultAssemblyResolver() });

    // 把 module 内所有 operand 指向 m 的 call/callvirt 重定向到 wrapper（跳过 wrapper 自身）
    static void RedirectCalls(ModuleDefinition module, MethodDefinition m, MethodDefinition wrapper)
    {
        int n = 0;
        foreach (var t in module.GetTypes())
            foreach (var mm in t.Methods)
            {
                if (mm.Body == null || mm == wrapper) continue;
                foreach (var ins in mm.Body.Instructions)
                    if ((ins.OpCode == OpCodes.Call || ins.OpCode == OpCodes.Callvirt)
                        && ReferenceEquals(ins.Operand, m))
                    {
                        ins.Operand = wrapper;
                        n++;
                    }
            }
        if (n > 0) Console.WriteLine("[r122]   同模块调用点重定向 " + n + " 处 -> wrapper");
    }

    static void PatchOne(ModuleDefinition mod, TypeDefinition type, MethodDefinition m)
    {
        var ret = m.ReturnType;
        if (ret.MetadataType != MetadataType.Void) {
            Console.WriteLine("[r122] !! 返回类型非 void，跳过: " + m.FullName);
            skipped++; return;
        }
        if (m.Parameters.Count != 1 || m.Parameters[0].ParameterType.MetadataType != MetadataType.IntPtr) {
            Console.WriteLine("[r122] !! 签名非 (IntPtr)，跳过: " + m.FullName);
            skipped++; return;
        }
        if (!m.IsPInvokeImpl) {
            Console.WriteLine("[r122] !! 非 P/Invoke，跳过: " + m.FullName);
            skipped++; return;
        }

        // 1) 原 P/Invoke 改名 _Raw，保留 native 导出名（关键：否则 dlsym 找不到）
        string nativeEntry = m.PInvokeInfo != null && !string.IsNullOrEmpty(m.PInvokeInfo.EntryPoint)
            ? m.PInvokeInfo.EntryPoint : METHOD;
        m.Name = RAW;
        if (m.PInvokeInfo != null) m.PInvokeInfo.EntryPoint = nativeEntry;

        // 2) 新建同名 wrapper：if (klass != 0) _Raw(klass);
        var w = new MethodDefinition(METHOD,
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            mod.TypeSystem.Void);
        w.Parameters.Add(new ParameterDefinition(m.Parameters[0].ParameterType));
        var body = new MethodBody(w) { InitLocals = false };
        w.Body = body;
        var il = body.GetILProcessor();

        // klass==0 -> 跳到末尾 ret（no-op）；否则重新压参数调原生。
        // 注意 brtrue/brfalse 会弹出栈顶，跳转目标处必须重新 ldarg.0，否则栈下溢。
        var retNull = il.Create(OpCodes.Ret);
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Brfalse_S, retNull);  // klass == 0 -> no-op
        il.Emit(OpCodes.Ldarg_0);             // 重新压参数（brfalse 已弹出）
        il.Emit(OpCodes.Call, m);             // 调 _Raw
        il.Emit(OpCodes.Ret);
        il.Append(retNull);

        type.Methods.Add(w);

        // 3) 同模块内调用点（Cecil 读入时 operand 持有 m 对象，改名后指向 _Raw）重定向到 wrapper
        RedirectCalls(mod, m, w);

        patched++;
        Console.WriteLine("[r122] " + METHOD + "(IntPtr) -> null-guard wrapper（native EntryPoint=" + nativeEntry + "）");
    }

    static int Main(string[] argv)
    {
        if (argv.Length < 1) { Console.Error.WriteLine("usage: patch_class_init_nullguard <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = argv[0];
        try
        {
            var asm = Load(path);
            var mod = asm.MainModule;
            var t = mod.GetTypes().FirstOrDefault(x => x.FullName == TYPE);
            if (t == null) throw new Exception("找不到 " + TYPE);

            var raw = t.Methods.FirstOrDefault(x => x.Name == RAW);
            var wrapper = t.Methods.FirstOrDefault(x => x.Name == METHOD && !x.IsPInvokeImpl && x.HasBody);
            var orig = t.Methods.FirstOrDefault(x => x.Name == METHOD && x.IsPInvokeImpl && x.IsStatic
                && x.Parameters.Count == 1 && x.Parameters[0].ParameterType.MetadataType == MetadataType.IntPtr
                && x.ReturnType.MetadataType == MetadataType.Void);
            // 幂等：wrapper + _Raw 都在、且无残留同名 P/Invoke = 已注入
            if (raw != null && wrapper != null && orig == null) {
                Console.WriteLine("[r122] 已注入（存在 " + RAW + " 与同名 wrapper），跳过");
                asm.Dispose();
                return 0;
            }
            if (orig == null) throw new Exception("找不到 " + METHOD + "(IntPtr)->void P/Invoke，且未注入");
            if (raw != null || wrapper != null) {
                Console.WriteLine("[r122] !! 部分注入状态（raw=" + (raw!=null) + " wrapper=" + (wrapper!=null) + "），拒绝修改");
                asm.Dispose(); return 1;
            }

            PatchOne(mod, t, orig);

            string tmp = path + ".r122tmp";
            asm.Write(tmp);
            asm.Dispose();
            File.Copy(tmp, path, true);
            File.Delete(tmp);
            Console.WriteLine("[r122] 完成，改写 " + patched + " 个方法，跳过 " + skipped + " 个");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine("[r122] !! 补丁失败: " + e); return 1; }
    }
}
