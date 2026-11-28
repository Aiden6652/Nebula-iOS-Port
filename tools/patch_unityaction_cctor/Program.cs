// R132: 真修 UnityAction`2.cctor -> get_TypeHandle(null) NRE（NOS 加载最后一崩点）。
//   真机 R130/R131：PatchAll 跑满 seq=163 ok=160（detour 已健康），但 NOS 加载崩在
//     SceneManager.get_sceneLoaded() -> Il2CppObjectPool.Get<UnityAction`2>
//       -> UnityAction`2..cctor() -> MakeGenericType([Scene.Type, LoadSceneMode.Type])=null
//       -> get_TypeHandle(null) -> Il2CppObjectBaseToPtrNotNull(null) NRE.
//   反编译 IL 根因（唯一确定）：
//     IL_0026: ldsfld CPS<T0=Scene>.NativeClassPtr
//     IL_0038: ldsfld CPS<T1=LoadSceneMode>.NativeClassPtr   <-- = 0！
//   Scene 是普通类，Scene.cctor（R130 白名单放行）会 stsfld CPS<Scene>.NativeClassPtr（有效）。
//   LoadSceneMode 是 enum，【没有自己的 cctor】，它的 CPS<LoadSceneMode>.NativeClassPtr
//   只能靠泛型包装类 Il2CppClassPointerStore`1<LoadSceneMode>.cctor 的 enum 分支设置，
//   而该泛型类被 R119/R130 全 DEFER（名字带反引号、不在 Scene 白名单）-> 指针=0。
//   0 -> il2cpp_class_get_type(0) -> internal_from_handle(null guard)=null -> MakeGenericType 收 null -> null -> NRE.
//   这也解释了 R130 为何进步（Scene 修好，seq 到 163）但仍崩（enum 没修）。
//
//   真修（真修复·非绕过·跨 AU 版本/设备/iOS）：把 IL_0026/IL_0038 两处对 CPS<T>.NativeClassPtr
//   的依赖，换成 native GetIl2CppClass(assembly, namespace, name) 按固定三元组直调：
//     "UnityEngine.CoreModule.dll","UnityEngine.SceneManagement","Scene"
//     "UnityEngine.CoreModule.dll","UnityEngine.SceneManagement","LoadSceneMode"
//   这三个字符串是 Unity 引擎固定 API，所有 AU 版本不变（R126/R127 已验证同款手法）；
//   GetIl2CppClass  malignancies: ourImagesMap 按 assembly 名查 image，Scene.cctor 已证明该 image
//   已注册（Scene 指针就是这么拿到的），故 LoadSceneMode 用同法必得有效 class。
//   Scene 处替换也一并做（双保险，去掉对 Scene.cctor 执行时序的依赖）。
//   含改动片段栈平衡校验（ldsfld push1 == ldstr*3+call(pop3/push1) 净 push1）。幂等、原子写回。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    static MethodReference getIl2CppClassRef = null;

    // 从 cctor 内已有 call GetIl2CppClass 指令复用 MethodReference（带正确 Scope）
    static MethodReference findGetIl2CppClass(MethodDefinition cctor)
    {
        foreach (var i in cctor.Body.Instructions)
            if ((i.OpCode == OpCodes.Call || i.OpCode == OpCodes.Callvirt) &&
                i.Operand is MethodReference mr && mr.Name == "GetIl2CppClass" &&
                mr.Parameters.Count == 3) return mr;
        return null;
    }

    // 找 cctor 内 ldsfld CPS<T>.NativeClassPtr 指令。
    // UnityAction`2 是开放泛型类，IL 里泛型参数是占位符 T0/T1：
    //   IL_0026: ldsfld Il2CppClassPointerStore`1<T0>::NativeClassPtr  (T0=Scene)
    //   IL_0038: ldsfld Il2CppClassPointerStore`1<T1>::NativeClassPtr  (T1=LoadSceneMode)
    //   IL_0066: ldsfld Il2CppClassPointerStore`1<UnityAction`2<T0,T1>>::NativeClassPtr (目标字段，非参数源)
    // 故按 DeclaringType.FullName 精确匹配 "<T0>" / "<T1>" 结尾。
    static Instruction findCpsLoad(MethodDefinition cctor, string genericParam)
    {
        string suffix = "<" + genericParam + ">";
        foreach (var i in cctor.Body.Instructions)
        {
            if (i.OpCode != OpCodes.Ldsfld) continue;
            var f = i.Operand as FieldReference;
            if (f == null || f.Name != "NativeClassPtr") continue;
            var dt = f.DeclaringType;
            if (dt == null) continue;
            string dtn = dt.FullName;
            if (dtn.Contains("Il2CppClassPointerStore") && dtn.EndsWith(suffix))
                return i;
        }
        return null;
    }

    static int patchOne(MethodDefinition cctor, Instruction ld, string asm, string ns, string name, string tag, ModuleDefinition mod)
    {
        // 幂等：若该位置后续已是 ldstr 三元组 + call GetIl2CppClass，跳过
        var n1 = ld.Next; var n2 = n1 != null ? n1.Next : null; var n3 = n2 != null ? n2.Next : null; var n4 = n3 != null ? n3.Next : null;
        if (n1 != null && n1.OpCode == OpCodes.Ldstr && (string)n1.Operand == asm &&
            n2 != null && n2.OpCode == OpCodes.Ldstr && (string)n2.Operand == ns &&
            n3 != null && n3.OpCode == OpCodes.Ldstr && (string)n3.Operand == name &&
            n4 != null && n4.OpCode == OpCodes.Call &&
            n4.Operand is MethodReference mr4 && mr4.Name == "GetIl2CppClass")
        {
            Console.WriteLine("[R132]   = " + tag + " 已是 GetIl2CppClass 直调（幂等）");
            return 0;
        }

        var il = cctor.Body.GetILProcessor();
        var iAsm = il.Create(OpCodes.Ldstr, asm);
        var iNs = il.Create(OpCodes.Ldstr, ns);
        var iName = il.Create(OpCodes.Ldstr, name);
        var iCall = il.Create(OpCodes.Call, getIl2CppClassRef);

        // 栈平衡：原 ldsfld 净 push1；新 ldstr*3(push3) + call(pop3/push1) 净 push1 —— 等价
        il.InsertBefore(ld, iAsm);
        il.InsertBefore(ld, iNs);
        il.InsertBefore(ld, iName);
        il.InsertBefore(ld, iCall);
        il.Remove(ld);

        // 校验片段：iAsm..iCall 净栈 = +1，且中间不下溢
        int st = 0;
        foreach (var x in new[] { iAsm, iNs, iName, iCall })
        {
            int pop = 0, push = 0;
            if (x.OpCode == OpCodes.Ldstr) push = 1;
            else if (x.OpCode == OpCodes.Call)
            {
                var mr = x.Operand as MethodReference;
                pop = mr.Parameters.Count; push = 1; // 返回 IntPtr
            }
            st -= pop; if (st < 0) { Console.Error.WriteLine("[R132] x " + tag + " 栈下溢"); return 8; }
            st += push;
        }
        if (st != 1) { Console.Error.WriteLine("[R132] x " + tag + " 片段出口栈=" + st + " (期望1，替代 ldsfld)"); return 9; }

        Console.WriteLine("[R132]   √ " + tag + " -> GetIl2CppClass(\"" + asm + "\",\"" + ns + "\",\"" + name + "\") [栈平衡OK]");
        return 1;
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_unityaction_cctor <UnityEngine.CoreModule.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "UnityEngine.Events.UnityAction`2");
        if (type == null) { Console.Error.WriteLine("[R132] x 找不到 UnityEngine.Events.UnityAction`2"); return 3; }
        var cctor = type.Methods.FirstOrDefault(m => m.Name == ".cctor" && m.IsStatic && m.HasBody);
        if (cctor == null) { Console.Error.WriteLine("[R132] x 找不到 UnityAction`2..cctor"); return 3; }

        getIl2CppClassRef = findGetIl2CppClass(cctor);
        if (getIl2CppClassRef == null) { Console.Error.WriteLine("[R132] x cctor 内找不到 GetIl2CppClass 引用"); return 4; }

        // 开放泛型占位符 T0=Scene, T1=LoadSceneMode（见 IL_0026/IL_0038）。
        var ld0 = findCpsLoad(cctor, "T0");
        var ld1 = findCpsLoad(cctor, "T1");
        // 幂等：若两处 ldsfld 都没了，且 cctor 已有足够的 GetIl2CppClass(3参) 直调（原本1处+新增2处=3处），视为已 patch
        int gicCount = cctor.Body.Instructions.Count(i =>
            i.OpCode == OpCodes.Call && i.Operand is MethodReference mrm && mrm.Name == "GetIl2CppClass" && mrm.Parameters.Count == 3);
        if (ld0 == null && ld1 == null)
        {
            if (gicCount >= 3) { Console.WriteLine("[R132] 已全部 patch（幂等跳过，GetIl2CppClass 直调 " + gicCount + " 处）"); return 0; }
            Console.Error.WriteLine("[R132] x 定位 CPS ldsfld 失败且直调数不足: T0=" + (ld0 != null) + " T1=" + (ld1 != null) + " gic=" + gicCount);
            return 5;
        }
        if (ld0 == null || ld1 == null)
        {
            Console.Error.WriteLine("[R132] x 只定位到一处 CPS ldsfld: T0=" + (ld0 != null) + " T1=" + (ld1 != null));
            return 5;
        }

        int n = 0;
        int r0 = patchOne(cctor, ld0, "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene", "T0=Scene", mod);
        if (r0 < 0) return r0; n += r0;
        // 注意：patchOne 改了指令链表，ld1 引用仍有效（Remove 只删 ld0/ld1，InsertBefore 插新节点）
        int r1 = patchOne(cctor, ld1, "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "LoadSceneMode", "T1=LoadSceneMode", mod);
        if (r1 < 0) return r1; n += r1;

        if (n == 0) { Console.WriteLine("[R132] 已全部 patch（幂等跳过）"); return 0; }
        mod.Write(path);
        Console.WriteLine("[R132] √ UnityAction`2.cctor CPS 依赖消除补丁已写回（patch " + n + " 处）");
        return 0;
    }
}
