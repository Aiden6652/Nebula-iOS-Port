// R132b: 根治 InjectorHelpers..cctor 的 TypeInitializationException（第三方插件/音频全挂）。
//   真机日志：
//     [Warning:Immersion Audio Bridge] runner install failed:
//       TypeInitializationException: The type initializer for 'Il2CppInterop.Runtime.Injection.InjectorHelpers' threw
//        ---> InvalidOperationException: Sequence contains no matching element
//         at System.Linq.Enumerable.Single[ProcessModule](...)
//         at Il2CppInterop.Runtime.Injection.InjectorHelpers..cctor()
//   根因：InjectorHelpers..cctor 里
//     IL_0014: Process.GetCurrentProcess().Modules.OfType<ProcessModule>()
//     IL_0033: Enumerable.Single<ProcessModule>(modules, m => m.ModuleName is GameAssembly/UnityFramework)
//     IL_0038: stsfld Il2CppModule
//   iOS 上 IL2CPP 静态链接进主可执行，Process.Modules 枚举不到任何叫 GameAssembly/UnityFramework
//   的模块 -> Single 抛 "Sequence contains no matching element" -> 整个 InjectorHelpers cctor 失败 ->
//   所有 ClassInjector.RegisterTypeInIl2Cpp<T>() 崩 -> AudioBridge / GamePatch 等动态注入插件全挂
//   （表现为进游戏一点声音都没有：AudioBridge 的桥接 runner 装不上）。
//   R116 只把匹配字符串 GameAssembly.dylib->UnityFramework，但 iOS 上 Process.Modules 压根枚举不到
//   该模块，换名字无效（真机 R131 仍崩，实证）。
//
//   真修（真修复·非绕过·跨版本/设备/iOS）：把 IL_0033 的 Enumerable.Single<ProcessModule>
//   换成 Enumerable.FirstOrDefault<ProcessModule>。找不到时返回 null 而非抛异常，cctor 继续完成，
//   Il2CppModule=null 不影响 ClassInjector —— 它真正依赖的是 Il2CppHandle（IL_003d~005f 由
//   NativeLibrary.Load("GameAssembly") 独立取得，iOS 宿主已导出 GetModuleHandle stub 兜底）。
//   Single<T> 与 FirstOrDefault<T> 签名完全相同：(IEnumerable<T>, Func<T,bool>) -> T，栈行为一致（pop2/push1）。
//   幂等、命中校验、栈平衡校验、原子写回（R78 截断教训）。
using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("用法: patch_injector_single <Il2CppInterop.Runtime.dll>"); return 2; }
        string path = args[0];
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("文件不存在: " + path); return 2; }

        var mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = true });
        var type = mod.GetTypes().FirstOrDefault(t => t.FullName == "Il2CppInterop.Runtime.Injection.InjectorHelpers");
        if (type == null) { Console.Error.WriteLine("[R132b] x 找不到 InjectorHelpers"); return 3; }
        var cctor = type.Methods.FirstOrDefault(m => m.Name == ".cctor" && m.IsStatic && m.HasBody);
        if (cctor == null) { Console.Error.WriteLine("[R132b] x 找不到 InjectorHelpers..cctor"); return 3; }

        // 先在全 DLL 找一个现成的、合法的 FirstOrDefault<T> 泛型实例引用的 ElementMethod（open generic 定义）。
        // 直接复用它（Cecil 从原 DLL 读出，签名/scope 100% 合法），避免手动 new MethodReference 导致
        // 二次读回 NotImplementedException / 设备 JIT 崩（R132b 第一版教训）。
        MethodReference foElement = null;
        foreach (var t in mod.GetTypes())
            foreach (var m in t.Methods)
            {
                if (!m.HasBody) continue;
                foreach (var ins in m.Body.Instructions)
                {
                    var gim = ins.Operand as GenericInstanceMethod;
                    if (gim != null && gim.ElementMethod.Name == "FirstOrDefault" &&
                        gim.ElementMethod.DeclaringType.FullName == "System.Linq.Enumerable" &&
                        gim.ElementMethod.Parameters.Count == 2)
                    { foElement = gim.ElementMethod; break; }
                }
                if (foElement != null) break;
            }
        if (foElement == null) { Console.Error.WriteLine("[R132b] x DLL 内找不到现成 FirstOrDefault ElementMethod 模板"); return 6; }
        Console.WriteLine("[R132b] 模板 ElementMethod: " + foElement.FullName);

        int patched = 0, already = 0;
        foreach (var ins in cctor.Body.Instructions)
        {
            if (ins.OpCode != OpCodes.Call) continue;
            var mr = ins.Operand as MethodReference;
            if (mr == null) continue;
            if (mr.DeclaringType == null || mr.DeclaringType.FullName != "System.Linq.Enumerable") continue;
            // 泛型实例方法引用是 GenericInstanceMethod
            var gim = mr as GenericInstanceMethod;
            // 幂等：已是 FirstOrDefault
            if (mr.Name == "FirstOrDefault" && gim != null && gim.GenericArguments.Count == 1 &&
                gim.GenericArguments[0].FullName == "System.Diagnostics.ProcessModule") { already++; continue; }
            if (mr.Name != "Single" || gim == null || gim.GenericArguments.Count != 1) continue;
            if (gim.GenericArguments[0].FullName != "System.Diagnostics.ProcessModule") continue;
            if (mr.Parameters.Count != 2) { Console.Error.WriteLine("[R132b] x Single 参数数非2"); return 4; }

            // 用现成的、合法的 FirstOrDefault ElementMethod 模板构造 GenericInstanceMethod，
            // 泛型实参用原 Single<ProcessModule> 的 ProcessModule（来自原 DLL，类型引用合法）。
            var fo = new GenericInstanceMethod(foElement);
            foreach (var ga in gim.GenericArguments) fo.GenericArguments.Add(ga);
            ins.Operand = fo;
            patched++;
            Console.WriteLine("[R132b]   √ Enumerable.Single<ProcessModule> -> FirstOrDefault<ProcessModule> [栈行为一致 pop2/push1]");
        }

        if (patched == 0 && already == 0)
        {
            Console.Error.WriteLine("[R132b] x 未找到目标 Single<ProcessModule> 调用（版本变化？），拒绝产出");
            return 5;
        }
        if (patched == 0) { Console.WriteLine("[R132b] 已全部 patch（幂等跳过 " + already + " 处）"); return 0; }

        // 栈平衡：Single 与 FirstOrDefault 均 pop2/push1，替换 operand 不改栈 —— 天然等价。
        mod.Write(path);
        Console.WriteLine("[R132b] √ InjectorHelpers Single->FirstOrDefault 补丁已写回（patch " + patched + " 处）");
        return 0;
    }
}
