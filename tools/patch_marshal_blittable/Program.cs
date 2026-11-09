using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using Mono.Cecil;
using Mono.Cecil.Cil;

// [R83] 修 iOS 裁剪版 MonoVM 的 marshal-ilgen stub 崩溃。
//
// 真机崩溃（nebula_host.txt, Loading [AutoSave] 时 SIGSEGV）：
//   Il2CppInterop.Runtime.IL2CPP::.cctor -> il2cpp_image_get_name_ 返回 string
//   -> mono_magic_trampoline -> emit_native_wrapper_ilgen
//   -> stub_emit_marshal_ilgen -> g_assert_not_reached() -> abort()
//
// 根因：这套 MonoVM 的 marshal-ilgen 后端被 stub 掉（build_ios.sh:122 早有记录
// "Cannot marshal nonblittlable types without marshal-ilgen"）。任何 P/Invoke 只要
// 签名里带 string / bool(marshal I1) 这类非 blittable 类型，首次 JIT 生成 native
// wrapper 就会 abort —— 与具体是 LPUTF8Str 还是 LPStr 无关，改枚举没用。
//
// 修法（对 Il2CppInterop.Runtime.dll 打 IL 补丁）：
//   对每个含 string/bool 的 il2cpp P/Invoke 方法 M：
//     1) 把 M 的签名改成全 blittable：string->IntPtr，bool->byte，
//        清掉 MarshalInfo，保留原来的 PInvokeInfo(Module=GameAssembly, EntryPoint)。
//        M 仍是 extern，指向同一个 il2cpp 导出，ABI 完全不变（1 字节 bool == byte）。
//     2) 新建一个与 M 同名、但用【原始签名】的普通静态方法 W。CLR 按
//        "名字 + 参数类型" 解析，所有旧调用点(call 的 MethodReference 是 string/bool
//        版)会自动落到 W 上 —— 调用点一行都不用改，Il2CppInterop.Generator、
//        HarmonySupport、131 个 interop 程序集全部不受影响。
//     3) W 的 IL：string 参数用 Marshal.StringToHGlobalAnsi 转指针（Unix 上 Ansi
//        == UTF-8，对 ASCII 类名/命名空间/方法名与 LPUTF8Str 逐字节一致），
//        bool 参数 conv.u1，调 blittable 版 M，finally 里 FreeHGlobal 释放，
//        string 返回值用 Marshal.PtrToStringAnsi，bool 返回值规范化为 0/1。
//
// 保存策略沿用 R82：InMemory + 写临时文件再原子替换，避免同路径写回把 DLL 截成 0 字节。
class PatchMarshalBlittable
{
    static ModuleDefinition mod;
    static TypeReference tIntPtr, tByte, tVoid, tString;
    static MethodReference mStrToPtr, mFreePtr, mPtrToStr;
    static int patched = 0, skipped = 0;

    static bool IsStr(TypeReference t) => t.MetadataType == MetadataType.String;
    static bool IsBool(TypeReference t) => t.MetadataType == MetadataType.Boolean;

    static TypeReference Blittable(TypeReference t) {
        if (IsStr(t))  return tIntPtr;
        if (IsBool(t)) return tByte;
        return t;
    }

    // 是否要处理：是 P/Invoke，且返回值或参数里有 string/bool；byref 的 string/bool 不碰
    static bool Needs(MethodDefinition m) {
        if (m.PInvokeInfo == null) return false;
        foreach (var p in m.Parameters) {
            var pt = p.ParameterType;
            if (pt is ByReferenceType br && (IsStr(br.ElementType) || IsBool(br.ElementType)))
                return false;   // 保守跳过，避免生成错误 IL
        }
        if (IsStr(m.ReturnType) || IsBool(m.ReturnType)) return true;
        return m.Parameters.Any(p => IsStr(p.ParameterType) || IsBool(p.ParameterType));
    }

    static void BuildMarshalRefs() {
        var scope = mod.TypeSystem.CoreLibrary;   // corlib / System.Runtime，Mono 端能解析
        var mt = new TypeReference("System.Runtime.InteropServices", "Marshal", mod, scope);
        mStrToPtr = new MethodReference("StringToHGlobalAnsi", tIntPtr, mt) { HasThis = false };
        mStrToPtr.Parameters.Add(new ParameterDefinition(tString));
        mFreePtr = new MethodReference("FreeHGlobal", tVoid, mt) { HasThis = false };
        mFreePtr.Parameters.Add(new ParameterDefinition(tIntPtr));
        mPtrToStr = new MethodReference("PtrToStringAnsi", tString, mt) { HasThis = false };
        mPtrToStr.Parameters.Add(new ParameterDefinition(tIntPtr));
    }

    // 按对象引用相等，把所有持有被改 P/Invoke 定义(m)的调用点重定向到 wrapper。
    // 跳过 wrapper 自身（其体内对 m 的 call 必须保留）。
    static void RedirectCalls(ModuleDefinition module, MethodDefinition m, MethodDefinition wrapper) {
        int n = 0;
        foreach (var t in module.GetTypes())
        foreach (var mm in t.Methods) {
            if (mm.Body == null || mm == wrapper) continue;
            foreach (var ins in mm.Body.Instructions) {
                if (ins.OpCode != OpCodes.Call && ins.OpCode != OpCodes.Calli &&
                    ins.OpCode != OpCodes.Callvirt && ins.OpCode != OpCodes.Newobj) continue;
                if (!ReferenceEquals(ins.Operand, m)) continue;
                ins.Operand = wrapper;   // 指向同签名 wrapper，返回/参数类型完全一致
                n++;
            }
        }
        if (n > 0) Console.WriteLine("[r83]   重定位 " + n + " 个调用点 -> " + wrapper.Name);
    }

    static void PatchOne(TypeDefinition type, MethodDefinition m) {
        // 快照原始签名
        var origParams = m.Parameters.Select(p => (TypeReference)p.ParameterType).ToList();
        TypeReference origRet = m.ReturnType;
        bool retStr = IsStr(origRet), retBool = IsBool(origRet);
        bool retVoid = origRet.MetadataType == MetadataType.Void;

        // 目标 blittable 签名若与本类已有方法撞车则跳过（防御）
        var newSig = origParams.Select(Blittable).ToList();
        if (type.Methods.Any(x => x.Name == m.Name && x != m &&
                x.Parameters.Count == newSig.Count &&
                x.Parameters.Select(p => p.ParameterType.FullName)
                    .SequenceEqual(newSig.Select(t => t.FullName)))) {
            Console.WriteLine("[r83] !! 跳过（blittable 签名撞车）: " + m.FullName);
            skipped++;
            return;
        }

        // ---- 1) 把 M 改成全 blittable 的 extern，并改名 _Raw 消除同名歧义 ----
        // 关键：改名后，所有旧调用点（引用 原始名+原始 string/bool 签名）只会解析到
        // 下面的 wrapper W，绝不会误绑到 blittable 版 M，避免 ret 类型不匹配。
        m.Name = m.Name + "_Raw";
        m.ReturnType = Blittable(origRet);
        m.MethodReturnType.MarshalInfo = null;
        for (int i = 0; i < m.Parameters.Count; i++) {
            m.Parameters[i].ParameterType = Blittable(origParams[i]);
            m.Parameters[i].MarshalInfo = null;
        }

        // ---- 2) 新建同名、原始签名的包装方法 W ----
        var w = new MethodDefinition(m.Name.Substring(0, m.Name.Length - 4),
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            origRet);
        for (int i = 0; i < origParams.Count; i++)
            w.Parameters.Add(new ParameterDefinition(origParams[i]));
        var body = new MethodBody(w) { InitLocals = true };
        w.Body = body;
        var il = body.GetILProcessor();

        // 为 string 参数准备指针 local
        var ptrLocals = new VariableDefinition[origParams.Count];
        var ptrList = new List<VariableDefinition>();
        for (int i = 0; i < origParams.Count; i++) {
            if (IsStr(origParams[i])) {
                var v = new VariableDefinition(tIntPtr);
                body.Variables.Add(v);
                ptrLocals[i] = v;
                ptrList.Add(v);
                il.Emit(OpCodes.Ldarg, i);
                il.Emit(OpCodes.Call, mStrToPtr);
                il.Emit(OpCodes.Stloc, v);
            }
        }

        // 结果 local（非 void 时）
        VariableDefinition resultLocal = null;
        if (!retVoid) {
            resultLocal = new VariableDefinition(origRet);
            body.Variables.Add(resultLocal);
        }

        Instruction tryStart = null, handlerStart = null, endInsn = null;

        // try 起点：压栈所有参数并调用 M
        for (int i = 0; i < origParams.Count; i++) {
            Instruction ins;
            if (IsStr(origParams[i]))      ins = il.Create(OpCodes.Ldloc, ptrLocals[i]);
            else if (IsBool(origParams[i])) ins = il.Create(OpCodes.Ldarg, i);
            else                            ins = il.Create(OpCodes.Ldarg, i);
            if (tryStart == null) tryStart = ins;
            il.Append(ins);
            if (IsBool(origParams[i])) il.Emit(OpCodes.Conv_U1);
        }
        il.Emit(OpCodes.Call, m);                       // 调 blittable 版原方法
        if (retStr)       il.Emit(OpCodes.Call, mPtrToStr);
        else if (retBool) { il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ceq);
                            il.Emit(OpCodes.Ldc_I4_0); il.Emit(OpCodes.Ceq); }
        if (!retVoid) il.Emit(OpCodes.Stloc, resultLocal);

        // finally：释放所有 string 指针
        if (ptrList.Count > 0) {
            // Leave 的操作数是 Instruction。先创建结束指令对象（此时还没 Append），
            // 用它当 leave 目标；finally 块写完后再把结束指令真正接到指令流末尾。
            endInsn = retVoid ? il.Create(OpCodes.Ret) : il.Create(OpCodes.Ldloc, resultLocal);
            il.Emit(OpCodes.Leave, endInsn);
            for (int k = 0; k < ptrList.Count; k++) {
                var ins = il.Create(OpCodes.Ldloc, ptrList[k]);
                if (handlerStart == null) handlerStart = ins;
                il.Append(ins);
                il.Emit(OpCodes.Call, mFreePtr);
            }
            il.Emit(OpCodes.Endfinally);
            il.Append(endInsn);
            if (!retVoid) il.Emit(OpCodes.Ret);
            body.ExceptionHandlers.Add(new ExceptionHandler(ExceptionHandlerType.Finally) {
                TryStart = tryStart, TryEnd = handlerStart,
                HandlerStart = handlerStart, HandlerEnd = endInsn
            });
        } else {
            // 没有 string 参数：不需要 try/finally，直接返回
            if (!retVoid) il.Emit(OpCodes.Ldloc, resultLocal);
            il.Emit(OpCodes.Ret);
        }

        type.Methods.Add(w);

        // ---- 3) 重定向同模块内所有调用点 ----
        // Cecil 读入时调用点的 operand 直接持有被改 P/Invoke 的 MethodDefinition 对象，
        // 我们原地改名/改签名后，那些调用点的 operand 也跟着变成 _Raw(blittable 版)，
        // 导致 ret 类型不匹配。这里按【对象引用相等】把这类调用点重定向到 wrapper W，
        // 并跳过 W 自身（W 内部对 _Raw 的 call 必须保留）。 ----
        RedirectCalls(mod, m, w);

        patched++;
        Console.WriteLine("[r83] " + m.Name + "(" +
            string.Join(",", origParams.Select(t => t.Name)) + ") -> " + origRet.Name +
            "  [string×" + ptrList.Count + ", bool=" + origParams.Count(IsBool) +
            (retStr ? ", ret=string" : "") + (retBool ? ", ret=bool" : "") + "]");
    }

    static int Main(string[] argv) {
        if (argv.Length < 1) {
            Console.Error.WriteLine("usage: patch_marshal_blittable <Il2CppInterop.Runtime.dll> [more.dll ...]");
            return 2;
        }
        try {
            foreach (var path in argv) {
                if (!File.Exists(path)) { Console.Error.WriteLine("[r83] 找不到 " + path); return 1; }
                var resolver = new DefaultAssemblyResolver();
                resolver.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
                var asm = AssemblyDefinition.ReadAssembly(path, new ReaderParameters {
                    ReadingMode = ReadingMode.Immediate, InMemory = true,
                    AssemblyResolver = resolver
                });
                mod = asm.MainModule;
                tIntPtr = mod.TypeSystem.IntPtr;
                tByte   = mod.TypeSystem.Byte;
                tVoid   = mod.TypeSystem.Void;
                tString = mod.TypeSystem.String;
                BuildMarshalRefs();

                var targets = new List<(TypeDefinition, MethodDefinition)>();
                foreach (var t in mod.GetTypes())
                    foreach (var m in t.Methods)
                        if (Needs(m)) targets.Add((t, m));

                Console.WriteLine("[r83] " + Path.GetFileName(path) + ": 命中 " + targets.Count + " 个非 blittable P/Invoke");
                foreach (var (t, m) in targets) PatchOne(t, m);

                string tmp = path + ".r83tmp";
                asm.Write(tmp);
                asm.Dispose();
                File.Copy(tmp, path, true);
                File.Delete(tmp);
            }
            Console.WriteLine("[r83] 完成：改写 " + patched + " 个方法，跳过 " + skipped + " 个");
            return 0;   // 已打过(patched=0)也视为成功，保证构建钩子可幂等重跑
        } catch (Exception e) {
            Console.Error.WriteLine("[r83] !! 补丁失败: " + e);
            return 1;
        }
    }
}
