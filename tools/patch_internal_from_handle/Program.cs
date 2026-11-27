// R126: 根治 Il2CppSystem.Type 的 icall 坏 methodInfo（iOS 预生成 interop 专属问题）。
//   internal_from_handle / op_Equality / op_Inequality 都是 System.Type 的 icall，
//   IL2CPP 里 icall 没有真正的 methodPointer，预生成 interop 用 token 解析出的
//   NativeMethodInfoPtr 是坏指针：iOS 上要么 SIGSEGV，要么被 R125 守卫拦成 null
//   后 op_Equality 的 il2cpp_object_unbox(null) NRE → Harmony.PatchAll 全空。
// 正解（上游新版 Il2CppInterop 做法，语义等价、跨版本/设备/iOS 通用）：
//   internal_from_handle(h) = Get<Type>(il2cpp_type_get_object(h))
//   op_Equality(a,b)        = Il2CppObjectBaseToPtr(a) == Il2CppObjectBaseToPtr(b)
//   op_Inequality(a,b)      = !(上面)
// 全部绕开坏 methodInfo，不依赖 R125 守卫。幂等、原子写回。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    static MethodReference findIl2cppType(TypeDefinition type, ModuleDefinition mod, string member)
    {
        // 从已有方法体里找 Il2CppInterop.Runtime.IL2CPP 的 TypeReference 或目标 MethodReference
        foreach (var t in mod.GetTypes())
            foreach (var md in t.Methods)
            {
                if (!md.HasBody) continue;
                foreach (var i in md.Body.Instructions)
                    if (i.Operand is MethodReference om && om.Name == member &&
                        om.DeclaringType != null && om.DeclaringType.FullName == "Il2CppInterop.Runtime.IL2CPP")
                        return om;
            }
        return null;
    }

    static TypeReference il2cppTypeRef(ModuleDefinition mod)
    {
        var r = findIl2cppType(null, mod, "il2cpp_runtime_invoke");
        if (r != null) return r.DeclaringType;
        var scope = mod.AssemblyReferences.FirstOrDefault(ar => ar.Name == "Il2CppInterop.Runtime")
                    ?? mod.AssemblyReferences.FirstOrDefault();
        return new TypeReference("Il2CppInterop.Runtime.IL2CPP", "Il2CppInterop.Runtime", mod, scope);
    }

    static int patchInternalFromHandle(ModuleDefinition mod, MethodDefinition m)
    {
        bool done = m.Body.Instructions.Any(i => i.OpCode == OpCodes.Call &&
            i.Operand is MethodReference mr && mr.Name == "il2cpp_type_get_object");
        if (done) { Console.WriteLine("[R126] internal_from_handle 已注入，跳过"); return 0; }

        MethodReference getObj = findIl2cppType(null, mod, "il2cpp_type_get_object");
        if (getObj == null)
        {
            getObj = new MethodReference("il2cpp_type_get_object", mod.TypeSystem.IntPtr, il2cppTypeRef(mod));
            getObj.HasThis = false;
            getObj.Parameters.Add(new ParameterDefinition(mod.TypeSystem.IntPtr));
        }

        MethodReference poolGet = null;
        foreach (var i in m.Body.Instructions)
            if (i.OpCode == OpCodes.Call && i.Operand is GenericInstanceMethod gim &&
                gim.Name == "Get" && gim.DeclaringType != null &&
                gim.DeclaringType.FullName.Contains("Il2CppObjectPool"))
            { poolGet = gim; break; }
        if (poolGet == null) { Console.Error.WriteLine("[R126] ✗ 找不到 Il2CppObjectPool.Get"); return 4; }

        var body = m.Body;
        body.Instructions.Clear(); body.Variables.Clear(); body.ExceptionHandlers.Clear();
        var vObj = new VariableDefinition(mod.TypeSystem.IntPtr);
        body.Variables.Add(vObj);
        var ilp = body.GetILProcessor();
        var ret = ilp.Create(OpCodes.Ret);
        var ok = ilp.Create(OpCodes.Call, poolGet);
        ilp.Append(ilp.Create(OpCodes.Ldarg_0));
        ilp.Append(ilp.Create(OpCodes.Call, getObj));
        ilp.Append(ilp.Create(OpCodes.Stloc, vObj));
        ilp.Append(ilp.Create(OpCodes.Ldloc, vObj));
        ilp.Append(ilp.Create(OpCodes.Brtrue_S, ok));
        ilp.Append(ilp.Create(OpCodes.Pop));
        ilp.Append(ilp.Create(OpCodes.Ldnull));
        ilp.Append(ilp.Create(OpCodes.Br_S, ret));
        ilp.Append(ok);
        ilp.Append(ret);
        Console.WriteLine("[R126] ✓ internal_from_handle → il2cpp_type_get_object");
        return 0;
    }

    static int patchEquality(ModuleDefinition mod, MethodDefinition m, bool isInequality)
    {
        string name = m.Name;
        bool done = m.Body.Instructions.Any(i =>
            i.OpCode == OpCodes.Ceq &&
            m.Body.Instructions.All(j => !(j.OpCode == OpCodes.Call &&
                j.Operand is MethodReference mr2 && mr2.Name == "il2cpp_runtime_invoke")));
        if (done) { Console.WriteLine($"[R126] {name} 已注入，跳过"); return 0; }

        MethodReference toPtr = findIl2cppType(null, mod, "Il2CppObjectBaseToPtr");
        if (toPtr == null) { Console.Error.WriteLine("[R126] ✗ 找不到 Il2CppObjectBaseToPtr"); return 5; }

        var body = m.Body;
        body.Instructions.Clear(); body.Variables.Clear(); body.ExceptionHandlers.Clear();
        var ilp = body.GetILProcessor();
        ilp.Append(ilp.Create(OpCodes.Ldarg_0));
        ilp.Append(ilp.Create(OpCodes.Call, toPtr));
        ilp.Append(ilp.Create(OpCodes.Ldarg_1));
        ilp.Append(ilp.Create(OpCodes.Call, toPtr));
        ilp.Append(ilp.Create(OpCodes.Ceq)); // a==b (native int)
        if (isInequality)
        {
            ilp.Append(ilp.Create(OpCodes.Ldc_I4_0));
            ilp.Append(ilp.Create(OpCodes.Ceq)); // !(a==b)
        }
        ilp.Append(ilp.Create(OpCodes.Ret));
        Console.WriteLine($"[R126] ✓ {name} → 指针相等比较（绕开 icall 坏 methodInfo）");
        return 0;
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_internal_from_handle <Il2Cppmscorlib.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppSystem.Type");
        if (type == null) { Console.Error.WriteLine("[R126] ✗ 找不到 Il2CppSystem.Type"); return 3; }

        int rc = 0;
        var ifh = type.Methods.FirstOrDefault(md => md.Name == "internal_from_handle" && md.Parameters.Count == 1);
        if (ifh == null) { Console.Error.WriteLine("[R126] ✗ 找不到 internal_from_handle"); return 3; }
        rc |= patchInternalFromHandle(mod, ifh);

        var eq = type.Methods.FirstOrDefault(md => md.Name == "op_Equality" && md.Parameters.Count == 2);
        if (eq != null) rc |= patchEquality(mod, eq, false);
        else Console.WriteLine("[R126] 未找到 op_Equality（跳过）");

        var neq = type.Methods.FirstOrDefault(md => md.Name == "op_Inequality" && md.Parameters.Count == 2);
        if (neq != null) rc |= patchEquality(mod, neq, true);
        else Console.WriteLine("[R126] 未找到 op_Inequality（跳过）");

        if (rc != 0) return rc;
        mod.Write(path);
        Console.WriteLine("[R126] ✓ 全部 icall 坏桩已根治并写回");
        return 0;
    }
}
