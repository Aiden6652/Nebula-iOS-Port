// [R78] 把 BepInEx Il2CppInteropManager::PreloadInteropAssemblies 里的
//   Parallel.ForEach<string>(IEnumerable<string>, Action<string>)
// 换成【顺序】版本，消除装插件阶段的线程池 worker。
//
// 为什么必须改：本内嵌 mono 用不了 MAP_JIT（真机 errno=1），W^X 靠进程级
// mprotect 翻页（set_write 一次翻所有 JIT 区）。线程 A 编译时把全区翻成 RW
// (无 EXEC)，线程 B 正在执行该区 JIT 码 → SIGSEGV。Parallel.ForEach 起 worker
// 后必然触发「并发 JIT」，故阶段B 必崩（R60~R76 七轮崩点一致）。
//
// 做法（语义等价、零新线程、零 EH）：
//   注入 public static class BepInEx.Unity.IL2CPP.NebulaSequential {
//     static void ForEach(IEnumerable<string> src, Action<string> act) {
//       new List<string>(src).ForEach(act);   // 顺序逐项调用，主线程完成
//     }
//   }
//   然后把原 call Parallel::ForEach 的操作数改指向该 helper（同为静态方法、
//   call 指令、参数栈 [IEnumerable,Action] 完全一致），并把紧随的 pop 改 nop
//   （Parallel.ForEach 返回 ParallelLoopResult 需弹栈，helper 返回 void 不需）。
using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class P {
  static int Main(string[] argv) {
    if (argv.Length < 1) { Console.Error.WriteLine("usage: patch <BepInEx.Unity.IL2CPP.dll>"); return 2; }
    string path = argv[0];
    // ReadingMode.Immediate：强制立即解析所有方法体，否则写回时 Cecil 会尝试
    // PatchRawMethodBody 从已关闭的原流读取 → EndOfStreamException + 把目标文件截成 0 字节。
    // InMemory：把整份读进内存，写回临时文件后再原子替换，绝不在读的同时写同一路径。
    var asm = AssemblyDefinition.ReadAssembly(path, new ReaderParameters {
        ReadingMode = ReadingMode.Immediate, InMemory = true, ReadWrite = false });
    var mod = asm.MainModule;

    // 幂等：已打过补丁就跳过
    if (mod.Types.Any(t => t.FullName == "BepInEx.Unity.IL2CPP.NebulaSequential")) {
      Console.WriteLine("[r78] 已打过顺序补丁，跳过"); return 0;
    }

    var mgr = mod.Types.FirstOrDefault(t => t.FullName == "BepInEx.Unity.IL2CPP.Il2CppInteropManager");
    if (mgr == null) { Console.Error.WriteLine("!! 找不到 Il2CppInteropManager"); return 3; }
    var preload = mgr.Methods.FirstOrDefault(m => m.Name == "PreloadInteropAssemblies" && m.HasBody);
    if (preload == null) { Console.Error.WriteLine("!! 找不到 PreloadInteropAssemblies"); return 4; }

    // 定位 Parallel.ForEach 调用
    var call = preload.Body.Instructions.FirstOrDefault(i =>
        (i.OpCode == OpCodes.Call || i.OpCode == OpCodes.Callvirt) &&
        i.Operand is MethodReference mr &&
        mr.DeclaringType.FullName == "System.Threading.Tasks.Parallel" && mr.Name == "ForEach");
    if (call == null) { Console.Error.WriteLine("!! 找不到 Parallel.ForEach 调用"); return 5; }
    Console.WriteLine($"[r78] 命中: IL_{call.Offset:x4} {call.Operand}");

    // 泛型实例：IEnumerable<string> / Action<string> / List<string>
    var str = mod.TypeSystem.String;
    var ienumT = new GenericInstanceType(mod.ImportReference(typeof(System.Collections.Generic.IEnumerable<>)));
    ienumT.GenericArguments.Add(str);
    var actionT = new GenericInstanceType(mod.ImportReference(typeof(System.Action<>)));
    actionT.GenericArguments.Add(str);
    var listT = new GenericInstanceType(mod.ImportReference(typeof(System.Collections.Generic.List<>)));
    listT.GenericArguments.Add(str);

    // 注入 helper 类型
    var seq = new TypeDefinition("BepInEx.Unity.IL2CPP", "NebulaSequential",
        TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.Class,
        mod.TypeSystem.Object);
    mod.Types.Add(seq);

    var helper = new MethodDefinition("ForEach",
        MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
        mod.TypeSystem.Void);
    helper.Parameters.Add(new ParameterDefinition("src", ParameterAttributes.None, ienumT));
    helper.Parameters.Add(new ParameterDefinition("act", ParameterAttributes.None, actionT));
    seq.Methods.Add(helper);

    // helper IL: new List<string>(src).ForEach(act); ret
    var listCtor = new MethodReference(".ctor", mod.TypeSystem.Void, listT) { HasThis = true };
    listCtor.Parameters.Add(new ParameterDefinition(ienumT));
    var listForEach = new MethodReference("ForEach", mod.TypeSystem.Void, listT) { HasThis = true };
    listForEach.Parameters.Add(new ParameterDefinition(actionT));
    var hp = helper.Body.GetILProcessor();
    hp.Append(hp.Create(OpCodes.Ldarg_0));
    hp.Append(hp.Create(OpCodes.Newobj, listCtor));
    hp.Append(hp.Create(OpCodes.Ldarg_1));
    hp.Append(hp.Create(OpCodes.Callvirt, listForEach));
    hp.Append(hp.Create(OpCodes.Ret));
    helper.Body.MaxStackSize = 2;

    // 改 call 操作数 -> helper（静态 call，参数栈一致）
    var helperRef = new MethodReference("ForEach", mod.TypeSystem.Void, seq) { HasThis = false };
    helperRef.Parameters.Add(new ParameterDefinition(ienumT));
    helperRef.Parameters.Add(new ParameterDefinition(actionT));
    call.OpCode = OpCodes.Call;
    call.Operand = helperRef;

    // 紧随的 pop（弹 ParallelLoopResult）-> nop
    var idx = preload.Body.Instructions.IndexOf(call);
    for (int k = idx + 1; k < Math.Min(idx + 4, preload.Body.Instructions.Count); k++) {
      var ins = preload.Body.Instructions[k];
      if (ins.OpCode == OpCodes.Pop) { ins.OpCode = OpCodes.Nop; ins.Operand = null; Console.WriteLine($"[r78] pop@n{idx+1}->nop @IL_{ins.Offset:x4}"); break; }
    }

    string tmp = path + ".r78.tmp";
    asm.Write(tmp);
    File.Move(tmp, path, true);   // 原子替换，避免写坏原件
    Console.WriteLine("[r78] 补丁写入成功: " + path);
    return 0;
  }
}
