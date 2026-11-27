// R126: 根治 Il2CppSystem.Type::internal_from_handle 在 iOS 上的坏 methodInfo 崩溃/NRE。
// 原实现：用 il2cpp_runtime_invoke 调 NativeMethodInfoPtr_internal_from_handle（icall），
//   该 method 指针在 iOS 设备端预生成 interop 里失效 → 要么 SIGSEGV（R123/R124），
//   要么被 R125 守卫拦成 null → 上游 op_Equality(null,...) NRE → patch 全空。
// 正解：internal_from_handle(handle) 的本质 = il2cpp_type_get_object(handle) 再包成
//   Il2CppSystem.Type 对象。上游新版 Il2CppInterop 就是这么生成该方法的，语义完全等价。
// 本工具把方法体整个替换为直接调 il2cpp_type_get_object，绕开坏 methodInfo，
//   不依赖 R125 守卫，跨 AU 版本/设备/iOS 通用。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_internal_from_handle <Il2Cppmscorlib.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var rp = new ReaderParameters { InMemory = true, ReadWrite = true };
        var mod = ModuleDefinition.ReadModule(path, rp);

        // 找到 Il2CppSystem.Type::internal_from_handle(IntPtr)
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppSystem.Type");
        if (type == null) { Console.Error.WriteLine("[R126] ✗ 找不到 Il2CppSystem.Type"); return 3; }
        var m = type.Methods.FirstOrDefault(md => md.Name == "internal_from_handle" && md.Parameters.Count == 1);
        if (m == null) { Console.Error.WriteLine("[R126] ✗ 找不到 internal_from_handle(IntPtr)"); return 3; }

        // 幂等：若已调用 il2cpp_type_get_object，跳过
        bool done = m.Body.Instructions.Any(i =>
            i.OpCode == OpCodes.Call &&
            i.Operand is MethodReference mr && mr.Name == "il2cpp_type_get_object");
        if (done) { Console.WriteLine("[R126] 已注入（internal_from_handle 已直调 il2cpp_type_get_object），跳过"); return 0; }

        // 找到 Il2CppInterop.Runtime.IL2CPP::il2cpp_type_get_object(IntPtr) 的引用
        MethodReference getObj = null;
        foreach (var t in mod.GetTypes())
            foreach (var md in t.Methods)
                if (md.Name == "il2cpp_type_get_object" && md.Parameters.Count == 1 &&
                    md.DeclaringType != null && md.DeclaringType.FullName == "Il2CppInterop.Runtime.IL2CPP")
                { getObj = md; break; }
        // 该 P/Invoke 可能在别的程序集（Il2CppInterop.Runtime.dll）。用 ImportReference 造跨程序集引用。
        if (getObj == null)
        {
            // 从已有引用里找 IL2CPP 类型引用（il2cpp_runtime_invoke 调用点）
            TypeReference il2cppTRef = null;
            foreach (var t in mod.GetTypes())
                foreach (var md in t.Methods)
                    if (!md.HasBody) continue;
                    else foreach (var i in md.Body.Instructions)
                        if (i.Operand is MethodReference om && om.Name == "il2cpp_runtime_invoke" &&
                            om.DeclaringType != null && om.DeclaringType.FullName == "Il2CppInterop.Runtime.IL2CPP")
                        { il2cppTRef = om.DeclaringType; break; }
            if (il2cppTRef == null)
            {
                // 兜底：用 Scope 指向 Il2CppInterop.Runtime 程序集
                var scope = mod.AssemblyReferences.FirstOrDefault(ar => ar.Name == "Il2CppInterop.Runtime")
                            ?? mod.AssemblyReferences.FirstOrDefault();
                il2cppTRef = new TypeReference("Il2CppInterop.Runtime.IL2CPP", "Il2CppInterop.Runtime", mod, scope);
            }
            getObj = new MethodReference("il2cpp_type_get_object", mod.TypeSystem.IntPtr, il2cppTRef);
            getObj.HasThis = false;
            getObj.Parameters.Add(new ParameterDefinition(mod.TypeSystem.IntPtr));
        }

        // 找到 Il2CppObjectPool.Get<Il2CppSystem.Type>(IntPtr) 泛型实例化引用（复用原方法体里的）
        MethodReference poolGet = null;
        foreach (var i in m.Body.Instructions)
            if (i.OpCode == OpCodes.Call && i.Operand is GenericInstanceMethod gim &&
                gim.Name == "Get" && gim.DeclaringType != null &&
                gim.DeclaringType.FullName.Contains("Il2CppObjectPool"))
            { poolGet = gim; break; }
        if (poolGet == null) { Console.Error.WriteLine("[R126] ✗ 找不到 Il2CppObjectPool.Get 引用"); return 4; }

        // 重建方法体：
        //   ldarg.0
        //   call il2cpp_type_get_object(IntPtr)   -> IntPtr typeObj
        //   stloc.0
        //   ldloc.0; dup; brtrue.s ok
        //   pop; ldnull; br.s ret
        //   ok: call Get<Type>(IntPtr)
        //   ret: ret
        var body = m.Body;
        body.Instructions.Clear();
        body.Variables.Clear();
        body.ExceptionHandlers.Clear();
        var vObj = new VariableDefinition(mod.TypeSystem.IntPtr);
        body.Variables.Add(vObj);
        var ilp = body.GetILProcessor();
        var ret = ilp.Create(OpCodes.Ret);
        var ok = ilp.Create(OpCodes.Call, poolGet);
        var brTrue = ilp.Create(OpCodes.Brtrue_S, ok);
        var brRet = ilp.Create(OpCodes.Br_S, ret);
        ilp.Append(ilp.Create(OpCodes.Ldarg_0));
        ilp.Append(ilp.Create(OpCodes.Call, getObj));
        ilp.Append(ilp.Create(OpCodes.Stloc, vObj));
        ilp.Append(ilp.Create(OpCodes.Ldloc, vObj));
        ilp.Append(brTrue);
        ilp.Append(ilp.Create(OpCodes.Pop));
        ilp.Append(ilp.Create(OpCodes.Ldnull));
        ilp.Append(brRet);
        ilp.Append(ok);
        ilp.Append(ret);

        mod.Write(path);
        Console.WriteLine("[R126] ✓ internal_from_handle 已替换为直调 il2cpp_type_get_object（绕开坏 methodInfo）");
        return 0;
    }
}
