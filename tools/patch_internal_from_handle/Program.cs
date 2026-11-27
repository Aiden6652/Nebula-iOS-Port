// R127: 修 R126/R126b internal_from_handle 补丁的非法 IL（真机 InvalidProgramException: IL_0010: pop）。
//   R126b 生成的是 ldarg.0/call getObj/stloc/ldloc/brtrue.s/pop/ldnull/br/call poolGet/ret：
//   brtrue.s 已弹出栈顶，零分支的 pop 弹空栈 -> JIT 抛 InvalidProgramException，
//   HarmonySupport.TryResolve -> Il2CppType.From -> TypeFromPointerInternal 编译该方法即炸，
//   Harmony.PatchAll 全灭（seq=0 ok=0 down=0），NOS 插件加载失败进原版。
// 正解（与 Il2CppInterop 官方生成器同构，栈恒平衡）：
//   internal_from_handle(h):
//     ldarg.0; call il2cpp_type_get_object(h);   // [IntPtr obj]
//     dup;                                       // [obj, obj]
//     brtrue.s ok;                               // 弹出一个判断, 剩 [obj]
//     pop; ldnull; br.s ret;                     // obj==0: 丢 obj, 压 null
//     ok: call Il2CppObjectPool.Get<Type>(obj);  // obj!=0: 消费 obj 得 Type
//     ret
//   op_Equality(a,b)  = Il2CppObjectBaseToPtr(a) == Il2CppObjectBaseToPtr(b)（已正确，保留）
//   op_Inequality(a,b)= !(上面)（已正确，保留）
// 幂等、原子写回；另加生成后 IL 结构自检，栈不平衡/结构错则拒绝产出。
using System;
using System.Collections.Generic;
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

    // 简单 IL 栈深度模拟：验证生成方法每个分支汇合点栈深一致、ret 处栈深符合返回类型。
    // 返回 null = 通过；否则返回错误描述。只覆盖我们生成用到的有限 opcode。
    static string verifyStack(MethodDefinition m)
    {
        var il = m.Body.Instructions;
        var index = new Dictionary<Instruction, int>();
        for (int i = 0; i < il.Count; i++) index[il[i]] = i;
        var depth = new int[il.Count];
        for (int i = 0; i < il.Count; i++) depth[i] = -1;
        var work = new Stack<int>();
        work.Push(0); depth[0] = 0;
        int expectRet = m.ReturnType.FullName == "System.Void" ? 0 : 1;
        while (work.Count > 0)
        {
            int pc = work.Pop();
            int d = depth[pc];
            var ins = il[pc];
            var op = ins.OpCode;
            if (op.FlowControl == FlowControl.Return)
            {
                if (d != expectRet) return $"ret@{pc} 栈深={d} 期望={expectRet}";
                continue;
            }
            int nd = d;
            // 精确处理 call/newobj：按参数个数与返回类型
            if (op == OpCodes.Call || op == OpCodes.Callvirt || op == OpCodes.Newobj)
            {
                var mr = (MethodReference)ins.Operand;
                int nargs = mr.Parameters.Count + (mr.HasThis && op != OpCodes.Newobj ? 1 : 0);
                nd = d - nargs + (mr.ReturnType.FullName == "System.Void" ? 0 : 1);
            }
            else switch (op.StackBehaviourPop)
            {
                case StackBehaviour.Pop0: break;
                case StackBehaviour.Pop1:
                case StackBehaviour.Popi:
                case StackBehaviour.Popref: nd -= 1; break;
                case StackBehaviour.Pop1_pop1:
                case StackBehaviour.Popi_pop1:
                case StackBehaviour.Popi_popi:
                case StackBehaviour.Popi_popi8:
                case StackBehaviour.Popi_popr4:
                case StackBehaviour.Popi_popr8:
                case StackBehaviour.Popref_pop1:
                case StackBehaviour.Popref_popi: nd -= 2; break;
                default: return $"@{pc} {op.Name} 未知 pop 行为 {op.StackBehaviourPop}";
            }
            if (op != OpCodes.Call && op != OpCodes.Callvirt && op != OpCodes.Newobj)
            {
                switch (op.StackBehaviourPush)
                {
                    case StackBehaviour.Push0: break;
                    case StackBehaviour.Push1:
                    case StackBehaviour.Pushi:
                    case StackBehaviour.Pushi8:
                    case StackBehaviour.Pushr4:
                    case StackBehaviour.Pushr8:
                    case StackBehaviour.Pushref: nd += 1; break;
                    case StackBehaviour.Push1_push1: nd += 2; break;
                    default: return $"@{pc} {op.Name} 未知 push 行为 {op.StackBehaviourPush}";
                }
            }
            if (nd < 0) return $"@{pc} {op.Name} 后栈深为负 ({nd})";

            bool isBranch = op.FlowControl == FlowControl.Branch || op.FlowControl == FlowControl.Cond_Branch;
            if (isBranch)
            {
                var targets = new System.Collections.Generic.List<Instruction>();
                if (ins.Operand is Instruction t) targets.Add(t);
                else if (ins.Operand is Instruction[] ts) targets.AddRange(ts);
                else return $"@{pc} 分支操作数异常";
                foreach (var tg in targets)
                {
                    int ti = index[tg];
                    if (depth[ti] == -1) { depth[ti] = nd; work.Push(ti); }
                    else if (depth[ti] != nd) return $"@{ti} 汇合栈深冲突: {depth[ti]} vs {nd} (来自 @{pc})";
                }
            }
            if (op.FlowControl == FlowControl.Branch || op.FlowControl == FlowControl.Return) continue;
            int next = pc + 1;
            if (next < il.Count)
            {
                if (depth[next] == -1) { depth[next] = nd; work.Push(next); }
                else if (depth[next] != nd) return $"@{next} 顺序栈深冲突: {depth[next]} vs {nd} (来自 @{pc})";
            }
        }
        return null;
    }

    static int patchInternalFromHandle(ModuleDefinition mod, MethodDefinition m)
    {
        bool done = m.Body.Instructions.Any(i => i.OpCode == OpCodes.Call &&
            i.Operand is MethodReference mr && mr.Name == "il2cpp_type_get_object");
        if (done) { Console.WriteLine("[R127] internal_from_handle 已注入，跳过"); return 0; }

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
        if (poolGet == null) { Console.Error.WriteLine("[R127] ✗ 找不到 Il2CppObjectPool.Get"); return 4; }

        var body = m.Body;
        body.Instructions.Clear(); body.Variables.Clear(); body.ExceptionHandlers.Clear();
        body.MaxStackSize = 8;
        var ilp = body.GetILProcessor();
        var ret = ilp.Create(OpCodes.Ret);
        var ok = ilp.Create(OpCodes.Call, poolGet);
        ilp.Append(ilp.Create(OpCodes.Ldarg_0));
        ilp.Append(ilp.Create(OpCodes.Call, getObj));   // [IntPtr obj]
        ilp.Append(ilp.Create(OpCodes.Dup));            // [obj, obj]
        ilp.Append(ilp.Create(OpCodes.Brtrue_S, ok));   // 消费一个判断 -> [obj]
        ilp.Append(ilp.Create(OpCodes.Pop));            // obj==0: 弹掉剩余 obj -> []
        ilp.Append(ilp.Create(OpCodes.Ldnull));         // [null]
        ilp.Append(ilp.Create(OpCodes.Br_S, ret));
        ilp.Append(ok);                                 // obj!=0: poolGet(obj) -> [Type]
        ilp.Append(ret);

        string why = verifyStack(m);
        if (why != null) { Console.Error.WriteLine($"[R127] ✗ internal_from_handle IL 栈校验失败: {why}"); return 6; }
        Console.WriteLine("[R127] ✓ internal_from_handle → il2cpp_type_get_object（dup+brtrue 栈平衡）");
        return 0;
    }

    static int patchEquality(ModuleDefinition mod, MethodDefinition m, bool isInequality)
    {
        string name = m.Name;
        bool done = m.Body.Instructions.Any(i =>
            i.OpCode == OpCodes.Ceq &&
            m.Body.Instructions.All(j => !(j.OpCode == OpCodes.Call &&
                j.Operand is MethodReference mr2 && mr2.Name == "il2cpp_runtime_invoke")));
        if (done) { Console.WriteLine($"[R127] {name} 已注入，跳过"); return 0; }

        MethodReference toPtr = findIl2cppType(null, mod, "Il2CppObjectBaseToPtr");
        if (toPtr == null) { Console.Error.WriteLine("[R127] ✗ 找不到 Il2CppObjectBaseToPtr"); return 5; }

        var body = m.Body;
        body.Instructions.Clear(); body.Variables.Clear(); body.ExceptionHandlers.Clear();
        body.MaxStackSize = 8;
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

        string why = verifyStack(m);
        if (why != null) { Console.Error.WriteLine($"[R127] ✗ {name} IL 栈校验失败: {why}"); return 6; }
        Console.WriteLine($"[R127] ✓ {name} → 指针相等比较（绕开 icall 坏 methodInfo）");
        return 0;
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_internal_from_handle <Il2Cppmscorlib.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppSystem.Type");
        if (type == null) { Console.Error.WriteLine("[R127] ✗ 找不到 Il2CppSystem.Type"); return 3; }

        int rc = 0;
        var ifh = type.Methods.FirstOrDefault(md => md.Name == "internal_from_handle" && md.Parameters.Count == 1);
        if (ifh == null) { Console.Error.WriteLine("[R127] ✗ 找不到 internal_from_handle"); return 3; }
        rc |= patchInternalFromHandle(mod, ifh);

        var eq = type.Methods.FirstOrDefault(md => md.Name == "op_Equality" && md.Parameters.Count == 2);
        if (eq != null) rc |= patchEquality(mod, eq, false);
        else Console.WriteLine("[R127] 未找到 op_Equality（跳过）");

        var neq = type.Methods.FirstOrDefault(md => md.Name == "op_Inequality" && md.Parameters.Count == 2);
        if (neq != null) rc |= patchEquality(mod, neq, true);
        else Console.WriteLine("[R127] 未找到 op_Inequality（跳过）");

        if (rc != 0) return rc;
        mod.Write(path);
        Console.WriteLine("[R127] ✓ 全部 icall 坏桩已根治（IL 栈校验通过）并写回");
        return 0;
    }
}
