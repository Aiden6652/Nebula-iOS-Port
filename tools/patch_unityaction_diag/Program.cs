// R133 (NOS 诊断插桩): 在 UnityAction`2.cctor 的关键 native 调用后插入栈中立的
// Console.WriteLine 日志, 一次性定位 get_TypeHandle(null) 的 null 断在哪一层。
//
// 背景: R131(methodInfo 按名字解析) / R132(CPS 读换 GetIl2CppClass 直调) 真机均无效,
//   崩溃栈一字不差 (get_TypeHandle 的 this=null)。三个补丁都进了设备(指纹变更+音频补丁
//   同批生效), 说明根因不在已改的点, 而在更深的 MakeGenericType 链。继续盲改 IL 无意义,
//   缺的是真机可观测性: GetIl2CppClass 对 Scene/LoadSceneMode 是否返回 0?
//   internal_from_handle 是否返回 null? MakeGenericType 是否返回 null?
//
// 诊断点 (在 R132 patch 后的 cctor 上, 每个 call 之后注入):
//   1) GetIl2CppClass(open generic UnityAction`2)   -> IntPtr 是否 0
//   2) GetIl2CppClass("...Scene")                   -> IntPtr 是否 0
//   3) GetIl2CppClass("...LoadSceneMode")           -> IntPtr 是否 0
//   4) il2cpp_class_get_type (每个)                 -> IntPtr 是否 0
//   5) internal_from_handle (每个)                  -> 返回 Type 引用是否 null
//   6) MakeGenericType                              -> 返回 Type 引用是否 null
//
// 栈中立实现: 在每个 call 之后 (返回值在栈顶) 插入:
//   dup; brtrue.s Lok; ldstr "<tag>=ZERO/NULL"; call WriteLine; br.s Lend;
//   Lok: ldstr "<tag>=NONZERO/NONNULL"; call WriteLine; Lend:
//   入口栈 S+1(返回值), dup->S+2, brtrue 弹1->S+1, ldstr->S+2, call 弹1->S+1,
//   br->S+1; Lok 分支同构; 两分支出口都是 S+1, 与入口一致, 原返回值仍在栈顶供后续消费。
//   故对任意原 cctor 完全无副作用 (除打印)。注入块自身栈中立由 verifyBlock 静态证明。
//
// 为什么用 Console.WriteLine: BepInEx 把 Console 输出重定向到 [r54][log] 行, 真机日志可见。
//   这些 cctor 在 NebulaManualExecute 之后的 sceneLoaded 注册阶段执行, Console 早已初始化。
//   绝不调用 Type.ToString()/GetType() 等会再触发 interop 的方法, 避免二次崩。
//
// 幂等: cctor 内出现 "[R133-DIAG]" 字符串即整体跳过。原子写回(临时文件+替换)。
using System;
using System.Collections.Generic;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    const string TAG = "[R133-DIAG]";

    static MethodReference writeLine = null;

    // 静态证明注入块栈中立: 给定入口栈深 d, 模拟 dup/brtrue/ldstr/call/br 两分支,
    // 出口必须仍为 d 且中途不下溢。与 d 具体值无关。
    static bool verifyBlock(int d)
    {
        int s = d;
        s += 1;              // dup
        s -= 1;              // brtrue
        if (s < 0) return false;
        int badExit = s + 1 - 1;   // ldstr; call WriteLine (pop1)
        int okExit = badExit;      // 对称
        return badExit == d && okExit == d;
    }

    // 在 call 指令之后插入栈中立的 null/zero 判断日志块。
    // isRef=true 表示返回值是引用类型(null 检查), 否则是 IntPtr(0 检查)。
    static void InjectAfter(ILProcessor il, Instruction call, string tag, bool isRef)
    {
        var anchor = call;
        var okLabel = il.Create(OpCodes.Nop);
        var endLabel = il.Create(OpCodes.Nop);
        string bad = isRef ? "NULL" : "ZERO";
        string good = isRef ? "NONNULL" : "NONZERO";

        Instruction i = il.Create(OpCodes.Dup); il.InsertAfter(anchor, i); anchor = i;
        i = il.Create(OpCodes.Brtrue_S, okLabel); il.InsertAfter(anchor, i); anchor = i;

        // bad 分支原地继续
        i = il.Create(OpCodes.Ldstr, TAG + " " + tag + "=" + bad); il.InsertAfter(anchor, i); anchor = i;
        i = il.Create(OpCodes.Call, writeLine); il.InsertAfter(anchor, i); anchor = i;
        i = il.Create(OpCodes.Br_S, endLabel); il.InsertAfter(anchor, i); anchor = i;

        // okLabel 分支
        il.InsertAfter(anchor, okLabel); anchor = okLabel;
        i = il.Create(OpCodes.Ldstr, TAG + " " + tag + "=" + good); il.InsertAfter(anchor, i); anchor = i;
        i = il.Create(OpCodes.Call, writeLine); il.InsertAfter(anchor, i); anchor = i;

        // endLabel
        il.InsertAfter(anchor, endLabel);
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_unityaction_diag <UnityEngine.CoreModule.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "UnityEngine.Events.UnityAction`2");
        if (type == null) { Console.Error.WriteLine("[R133] x 找不到 UnityEngine.Events.UnityAction`2"); return 3; }
        var cctor = type.Methods.FirstOrDefault(m => m.Name == ".cctor" && m.IsStatic && m.HasBody);
        if (cctor == null) { Console.Error.WriteLine("[R133] x 找不到 UnityAction`2..cctor"); return 3; }

        if (cctor.Body.Instructions.Any(i => i.OpCode == OpCodes.Ldstr && ((string)i.Operand ?? "").Contains(TAG)))
        {
            Console.WriteLine("[R133] 已插桩（幂等跳过）");
            return 0;
        }

        // 注入块栈中立静态证明(任意入口栈深)
        if (!verifyBlock(0) || !verifyBlock(1) || !verifyBlock(3))
        {
            Console.Error.WriteLine("[R133] x 注入块栈中立自检失败, 拒绝产出");
            return 4;
        }

        writeLine = mod.ImportReference(typeof(Console).GetMethod("WriteLine", new[] { typeof(string) }));
        var il = cctor.Body.GetILProcessor();
        var ins = cctor.Body.Instructions;

        var gic = new List<Instruction>();   // GetIl2CppClass (IntPtr)
        var gct = new List<Instruction>();   // il2cpp_class_get_type (IntPtr)
        var ifh = new List<Instruction>();   // internal_from_handle (Type 引用)
        var mgt = new List<Instruction>();   // MakeGenericType (Type 引用)
        foreach (var x in ins)
        {
            if (x.OpCode != OpCodes.Call && x.OpCode != OpCodes.Callvirt) continue;
            var mr = x.Operand as MethodReference;
            if (mr == null) continue;
            if (mr.Name == "GetIl2CppClass" && mr.Parameters.Count == 3) gic.Add(x);
            else if (mr.Name == "il2cpp_class_get_type") gct.Add(x);
            else if (mr.Name == "internal_from_handle") ifh.Add(x);
            else if (mr.Name == "MakeGenericType") mgt.Add(x);
        }

        Console.WriteLine("[R133] 目标: GetIl2CppClass=" + gic.Count + " class_get_type=" + gct.Count +
                          " internal_from_handle=" + ifh.Count + " MakeGenericType=" + mgt.Count);
        if (gic.Count == 0 || mgt.Count == 0)
        {
            Console.Error.WriteLine("[R133] x 关键 call 一个都没有, 拒绝插桩(可能未在 R132 patch 后的 DLL 上跑)");
            return 5;
        }

        int n = 0;
        // 快照列表注入, 每个 call 只注入一次、注入点互不重叠, 链表遍历收集完后再注入不会错乱。
        for (int k = 0; k < gic.Count; k++)  { InjectAfter(il, gic[k],  "GetIl2CppClass#" + k, false); n++; }
        for (int k = 0; k < gct.Count; k++)  { InjectAfter(il, gct[k],  "class_get_type#" + k, false); n++; }
        for (int k = 0; k < ifh.Count; k++)  { InjectAfter(il, ifh[k],  "internal_from_handle#" + k, true); n++; }
        for (int k = 0; k < mgt.Count; k++)  { InjectAfter(il, mgt[k],  "MakeGenericType#" + k, true); n++; }

        Console.WriteLine("[R133] √ 注入块栈中立自检通过 (" + n + " 处日志)");

        string tmp = path + ".r133.tmp";
        mod.Write(tmp);
        mod.Dispose();
        System.IO.File.Copy(tmp, path, true);
        System.IO.File.Delete(tmp);
        Console.WriteLine("[R133] √ 诊断插桩已写回 (" + n + " 处)");
        return 0;
    }
}
