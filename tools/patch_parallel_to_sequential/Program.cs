// [R79] 把 BepInEx Il2CppInteropManager::PreloadInteropAssemblies 里的
//   Parallel.ForEach<string>(IEnumerable<string>, Action<string>)
// 换成【顺序】版本，消除装插件阶段的线程池 worker。
//
// 为什么必须改：本内嵌 mono 用不了 MAP_JIT（真机 errno=1），W^X 靠进程级
// mprotect 翻页（set_write 一次翻所有 JIT 区）。线程 A 编译时把全区翻成 RW
// (无 EXEC)，线程 B 正在执行该区 JIT 码 → SIGSEGV。Parallel.ForEach 起 worker
// 后必然触发「并发 JIT」，故阶段B 必崩（R60~R76 七轮崩点一致）。
//
// [R79 修复 R78 的坑] R78 用 mod.ImportReference(typeof(List<>)) 生成 helper，
// Cecil 把类型 scope 导成了补丁工具自己的 System.Private.CoreLib
// (Version=6.0.0.0, PKT=7cec85d7bea7798e)。设备 mono 只认 BepInEx 原本引用的
// System.Runtime (PKT=b03f5f7f11d50a3a) facade，于是解析
//   List<string>::.ctor(IEnumerable<string>)  -> MissingMethodException
// PreloadInteropAssemblies 抛异常 → NebulaManualExecute 被吞 → 插件/NOS 全不加载
// （R78 真机现象：不崩、进大厅、但没 NOS）。
//
// R79 做法（语义等价、零新线程、零泛型集合成员、scope 与 BepInEx 完全一致）：
//   注入 public static class BepInEx.Unity.IL2CPP.NebulaSequential {
//     static void ForEach(IEnumerable<string> src, Action<string> act) {
//       var e = ((System.Collections.IEnumerable)src).GetEnumerator();
//       while (e.MoveNext()) act((string)e.Current);   // 非泛型接口 + castclass
//     }
//   }
//   参数类型直接复用原 IL 里 Directory.EnumerateFiles 的返回类型与
//   Action<string> 的 newobj 声明类型（都是 System.Runtime scope），不 import
//   任何东西。委托调用优先复用程序集里现成的 Action<string>::Invoke 引用。
//   最后把原 call Parallel::ForEach 的操作数改指向该 helper（同为静态 call、
//   参数栈 [IEnumerable,Action] 一致），紧随的 pop 改 nop（Parallel.ForEach
//   返回 ParallelLoopResult 需弹栈，helper 返回 void 不需）。
using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class P {
  static string Scope(TypeReference t) {
    try { return t.Scope == null ? "none" : t.Scope.ToString(); } catch { return "?"; }
  }

  static int Main(string[] argv) {
    if (argv.Length < 1) { Console.Error.WriteLine("usage: patch <BepInEx.Unity.IL2CPP.dll>"); return 2; }
    string path = argv[0];
    // ReadingMode.Immediate + InMemory：写回时 Cecil 不会从已关闭的原流
    // PatchRawMethodBody 读方法体（否则会把目标文件截成 0 字节）。
    var asm = AssemblyDefinition.ReadAssembly(path, new ReaderParameters {
        ReadingMode = ReadingMode.Immediate, InMemory = true, ReadWrite = false });
    var mod = asm.MainModule;

    var mgr = mod.Types.FirstOrDefault(t => t.FullName == "BepInEx.Unity.IL2CPP.Il2CppInteropManager");
    if (mgr == null) { Console.Error.WriteLine("!! 找不到 Il2CppInteropManager"); return 3; }
    var preload = mgr.Methods.FirstOrDefault(m => m.Name == "PreloadInteropAssemblies" && m.HasBody);
    if (preload == null) { Console.Error.WriteLine("!! 找不到 PreloadInteropAssemblies"); return 4; }

    // 复用原 IL 里已存在的类型引用（scope = System.Runtime），杜绝 import 错 scope
    var enumerate = preload.Body.Instructions.FirstOrDefault(i =>
        i.OpCode == OpCodes.Call && i.Operand is MethodReference mr &&
        mr.DeclaringType.FullName == "System.IO.Directory" && mr.Name == "EnumerateFiles");
    var actionNew = preload.Body.Instructions.FirstOrDefault(i =>
        i.OpCode == OpCodes.Newobj && i.Operand is MethodReference mr &&
        mr.DeclaringType is GenericInstanceType g && g.ElementType.FullName == "System.Action`1");
    if (enumerate == null || actionNew == null) { Console.Error.WriteLine("!! 找不到 EnumerateFiles / Action<string> 构造"); return 6; }
    var ienumT = (GenericInstanceType)((MethodReference)enumerate.Operand).ReturnType;
    var actionT = (GenericInstanceType)((MethodReference)actionNew.Operand).DeclaringType;
    Console.WriteLine($"[r79] ienumT = {ienumT.FullName} scope={Scope(ienumT.ElementType)}");
    Console.WriteLine($"[r79] actionT= {actionT.FullName} scope={Scope(actionT.ElementType)}");

    // 优先复用程序集里现成的 Action<string>::Invoke（签名/scope 已在真机验证可用）
    MethodReference invokeRef = null;
    foreach (var t in mod.Types) {
      foreach (var mm in t.Methods) {
        if (!mm.HasBody) continue;
        foreach (var ins in mm.Body.Instructions) {
          if (ins.Operand is MethodReference mr && mr.Name == "Invoke" &&
              mr.DeclaringType is GenericInstanceType git &&
              git.ElementType.FullName == "System.Action`1" &&
              git.GenericArguments.Count == 1 &&
              git.GenericArguments[0].FullName == "System.String") { invokeRef = mr; break; }
        }
        if (invokeRef != null) break;
      }
      if (invokeRef != null) break;
    }
    if (invokeRef != null) {
      Console.WriteLine($"[r79] 复用现成 Invoke: {invokeRef.FullName} scope={Scope(invokeRef.DeclaringType)}");
    } else {
      // 关键：MemberRef 签名里参数必须是 !0 (VAR 0)，写成具体 string 会让 CLR/mono
      // 去找 Action`1::Invoke(string) 而实际是 Invoke(!0) → MissingMethodException
      // （R79 首版在 .NET6 实测踩过，helpertest 当场报 Method not found）。
      // 又不能 new GenericParameter(owner)：Cecil 0.11.x 写裸 VAR 会抛
      // NotSupportedException。所以从原程序集里借一个现成的 VAR(0)
      // （IL_000b ConfigEntry<bool>::get_Value() 的返回值就是 !0），
      // 它在本 DLL 里本来就能被 Cecil 正常写回。
      TypeReference var0 = null;
      foreach (var t in mod.Types) {
        foreach (var mm in t.Methods) {
          if (!mm.HasBody) continue;
          foreach (var ins in mm.Body.Instructions) {
            if (!(ins.Operand is MethodReference mr2)) continue;
            TypeReference[] cands = new TypeReference[mr2.Parameters.Count + 1];
            cands[0] = mr2.ReturnType;
            for (int pi = 0; pi < mr2.Parameters.Count; pi++) cands[pi + 1] = mr2.Parameters[pi].ParameterType;
            foreach (var c in cands)
              if (c is GenericParameter gp && gp.MetadataType == MetadataType.Var) { var0 = gp; break; }
            if (var0 != null) break;
          }
          if (var0 != null) break;
        }
        if (var0 != null) break;
      }
      if (var0 == null) { Console.Error.WriteLine("!! 找不到可复用的 VAR(0) 签名"); return 8; }
      Console.WriteLine("[r79] 未找到现成 Invoke，按 System.Runtime scope + 复用 VAR(0) 构造");
      invokeRef = new MethodReference("Invoke", mod.TypeSystem.Void, actionT) { HasThis = true };
      invokeRef.Parameters.Add(new ParameterDefinition(var0));
    }

    var core = mod.TypeSystem.CoreLibrary;   // System.Runtime, PKT=b03f5f7f11d50a3a
    var ienumNT     = new TypeReference("System.Collections", "IEnumerable", mod, core);
    var enumeratorT = new TypeReference("System.Collections", "IEnumerator", mod, core);
    var getEnumerator = new MethodReference("GetEnumerator", enumeratorT, ienumNT) { HasThis = true };
    var moveNext      = new MethodReference("MoveNext", mod.TypeSystem.Boolean, enumeratorT) { HasThis = true };
    var getCurrent    = new MethodReference("get_Current", mod.TypeSystem.Object, enumeratorT) { HasThis = true };

    // 注入/复用 helper 类型
    var seq = mod.Types.FirstOrDefault(t => t.FullName == "BepInEx.Unity.IL2CPP.NebulaSequential");
    if (seq == null) {
      seq = new TypeDefinition("BepInEx.Unity.IL2CPP", "NebulaSequential",
          TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.Class,
          mod.TypeSystem.Object);
      mod.Types.Add(seq);
      Console.WriteLine("[r79] 注入 NebulaSequential 类型");
    } else {
      Console.WriteLine("[r79] 已存在 NebulaSequential，重建 ForEach 方法体（修复 R78 错 scope）");
    }
    var helper = seq.Methods.FirstOrDefault(m => m.Name == "ForEach");
    if (helper == null) {
      helper = new MethodDefinition("ForEach",
          MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
          mod.TypeSystem.Void);
      seq.Methods.Add(helper);
    }
    helper.Parameters.Clear();
    helper.Parameters.Add(new ParameterDefinition("src", ParameterAttributes.None, ienumT));
    helper.Parameters.Add(new ParameterDefinition("act", ParameterAttributes.None, actionT));
    helper.Body.Instructions.Clear();
    helper.Body.Variables.Clear();
    helper.Body.ExceptionHandlers.Clear();
    helper.Body.InitLocals = true;
    helper.Body.Variables.Add(new VariableDefinition(enumeratorT));

    // helper IL（不用 List / 不用泛型接口成员 / 不用 EH）：
    //   e = ((IEnumerable)src).GetEnumerator();
    //   goto CHECK;
    // BODY: act((string)e.Current);
    // CHECK: if (e.MoveNext()) goto BODY;
    //   ret;
    var hp = helper.Body.GetILProcessor();
    var check = hp.Create(OpCodes.Ldloc_0);
    var body  = hp.Create(OpCodes.Ldarg_1);
    hp.Append(hp.Create(OpCodes.Ldarg_0));
    hp.Append(hp.Create(OpCodes.Callvirt, getEnumerator));
    hp.Append(hp.Create(OpCodes.Stloc_0));
    hp.Append(hp.Create(OpCodes.Br_S, check));
    hp.Append(body);
    hp.Append(hp.Create(OpCodes.Ldloc_0));
    hp.Append(hp.Create(OpCodes.Callvirt, getCurrent));
    hp.Append(hp.Create(OpCodes.Castclass, mod.TypeSystem.String));
    hp.Append(hp.Create(OpCodes.Callvirt, invokeRef));
    hp.Append(check);
    hp.Append(hp.Create(OpCodes.Callvirt, moveNext));
    hp.Append(hp.Create(OpCodes.Brtrue_S, body));
    hp.Append(hp.Create(OpCodes.Ret));
    helper.Body.MaxStackSize = 2;

    // 改 call 操作数 -> helper（新 DLL 指向 Parallel.ForEach，旧 DLL 已指向 helper）
    var call = preload.Body.Instructions.FirstOrDefault(i =>
        (i.OpCode == OpCodes.Call || i.OpCode == OpCodes.Callvirt) && i.Operand is MethodReference mr &&
        ((mr.DeclaringType.FullName == "System.Threading.Tasks.Parallel" && mr.Name == "ForEach") ||
         (mr.DeclaringType.FullName == "BepInEx.Unity.IL2CPP.NebulaSequential" && mr.Name == "ForEach")));
    if (call == null) { Console.Error.WriteLine("!! 找不到 Parallel/NebulaSequential ForEach 调用"); return 5; }
    Console.WriteLine($"[r79] 命中: IL_{call.Offset:x4} {call.Operand}");
    var helperRef = new MethodReference("ForEach", mod.TypeSystem.Void, seq) { HasThis = false };
    helperRef.Parameters.Add(new ParameterDefinition(ienumT));
    helperRef.Parameters.Add(new ParameterDefinition(actionT));
    call.OpCode = OpCodes.Call;
    call.Operand = helperRef;

    var idx = preload.Body.Instructions.IndexOf(call);
    for (int k = idx + 1; k < Math.Min(idx + 4, preload.Body.Instructions.Count); k++) {
      var ins = preload.Body.Instructions[k];
      if (ins.OpCode == OpCodes.Pop) {
        ins.OpCode = OpCodes.Nop; ins.Operand = null;
        Console.WriteLine($"[r79] pop@n{idx+1}->nop @IL_{ins.Offset:x4}"); break;
      }
    }

    string tmp = path + ".r79.tmp";
    asm.Write(tmp);

    // 写回自检：helper 里绝不能再出现 List<> 或 System.Private.CoreLib scope
    var chk = AssemblyDefinition.ReadAssembly(tmp, new ReaderParameters {
        ReadingMode = ReadingMode.Immediate, InMemory = true });
    var hc = chk.MainModule.Types.First(t => t.FullName == "BepInEx.Unity.IL2CPP.NebulaSequential")
                 .Methods.First(m => m.Name == "ForEach");
    int bad = 0;
    foreach (var ins in hc.Body.Instructions) {
      if (ins.Operand is MethodReference mr) {
        string s = mr.FullName + " | " + Scope(mr.DeclaringType);
        if (s.Contains("System.Private.CoreLib") || s.Contains("System.Collections.Generic.List")) {
          Console.Error.WriteLine("[r79] !! 坏引用: " + s); bad++;
        }
      }
    }
    if (bad > 0) { Console.Error.WriteLine("[r79] !! 自检失败，丢弃产物"); File.Delete(tmp); return 7; }
    Console.WriteLine($"[r79] 自检通过：helper {hc.Body.Instructions.Count} 条指令，零 List / 零 System.Private.CoreLib");

    File.Move(tmp, path, true);   // 原子替换，避免写坏原件
    Console.WriteLine("[r79] 补丁写入成功: " + path);
    return 0;
  }
}
