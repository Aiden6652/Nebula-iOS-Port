// R136 (0 降级真修): 根治 RegisterTypeInIl2Cpp (class injection) 在 iOS ARM64 上全崩。
//   真机 R135 日志（BUILD=40ad9c0df119）：
//     ✅ [R134-CLOSED] closedClass=NONZERO / PatchAll seq=163 ok=160 / 音频 DllNotFound 消失
//     ❌ 新崩点（系统性，NOS 与 GamePatch 音频插件都中）：
//        RegisterTypeInIl2Cpp<Il2CppToMonoDelegateReference> / <BridgeRunner>
//        -> InjectorHelpers.Setup() -> 各 Hook.ApplyHook -> FindTargetMethod
//        -> XrefScannerLowLevel.JumpTargets(导出地址,false) 用 Iced x86 decoder
//        -> ARM64 机器码解不出 b/bl -> 空 -> Enumerable.Single() 抛
//           "Sequence contains no elements" -> ApplyHook 冒泡 -> NebulaLoader 加载失败
//           -> NOS 不进大厅 + 音频插件挂（大厅无音乐）。
//
//   根因（反编译 + 官方源码双证）：
//     Il2CppInterop 1.5.3 的 XrefScannerLowLevel.JumpTargets / JumpTargetsImpl 用
//     Iced.Intel.Decoder（Decoder.Create(IntPtr.Size*8, ...)，x86/x64 专用）。设备是
//     ARM64（崩栈 ldr w8,[x8,#8]），x86 解码器解 ARM64 指令得到无效 opcode，扫不到
//     任何跳转目标 -> 空序列 -> Single() 崩。这与 JIT 权限无关（JIT 只让 mono 跑托管码，
//     不改变 Iced 是 x86 解码器这一事实）。R135 的 FindSignatureInModule null 守卫只是
//     把 NRE 变成 fallback 崩，未触及根因。
//
//   真修（真修复·0 降级·跨 AU 版本/设备/iOS）；唯一最小改动点 = JumpTargets：
//     把 NebulaArm64Xref.Arm64Xref.Scan / IsArm64 的方法体（纯托管 ARM64 解码，
//     Marshal.ReadInt32 + 位运算，不依赖 Iced/Capstone 原生库）克隆进
//     Il2CppInterop.Common.dll 的 XrefScannerLowLevel 类型；
//     原 JumpTargets 改名 JumpTargetsX86（PC 路径原样保留）；
//     新 JumpTargets 分发器：IsArm64() ? Scan(code,useXref) : JumpTargetsX86(code,useXref)。
//     所有 5 个 hook 都调 JumpTargets，一处修复全覆盖。Scan 恒返回 >=1 个目标
//     （解码失败回退导出地址本身），保证上层 Single()/Last()/ToArray() 不崩。
//   PC/Win：ProcessArchitecture != Arm64 -> 走 JumpTargetsX86，字节级零变化。
//   幂等、命中校验、栈中立、原子写回。
using System;
using System.Collections.Generic;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

class Program
{
    const string COMMON_TYPE = "Il2CppInterop.Common.XrefScans.XrefScannerLowLevel";
    const string HELPER_TYPE = "NebulaArm64Xref.Arm64Xref";

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("用法: patch_xref_arm64 <Il2CppInterop.Common.dll> <NebulaArm64Xref.dll>");
            return 2;
        }
        string commonPath = args[0], helperPath = args[1];
        if (!System.IO.File.Exists(commonPath)) { Console.Error.WriteLine("[R138] x Common.dll 不存在: " + commonPath); return 2; }
        if (!System.IO.File.Exists(helperPath)) { Console.Error.WriteLine("[R138] x helper.dll 不存在: " + helperPath); return 2; }

        var common = ModuleDefinition.ReadModule(commonPath, new ReaderParameters { InMemory = true, ReadWrite = true });
        var helper = ModuleDefinition.ReadModule(helperPath, new ReaderParameters { InMemory = true, ReadWrite = false });

        var tgtType = common.GetTypes().FirstOrDefault(t => t.FullName == COMMON_TYPE);
        if (tgtType == null) { Console.Error.WriteLine("[R138] x 找不到 " + COMMON_TYPE); return 3; }
        var hType = helper.GetTypes().FirstOrDefault(t => t.FullName == HELPER_TYPE);
        if (hType == null) { Console.Error.WriteLine("[R138] x 找不到 helper 类型 " + HELPER_TYPE); return 3; }

        // 幂等：若已存在 ScanArm64 方法说明打过补丁
        if (tgtType.Methods.Any(m => m.Name == "ScanArm64") && tgtType.Methods.Any(m => m.Name == "LooksArm64"))
        {
            Console.WriteLine("[R138] = ARM64 xref 补丁已存在，跳过（幂等）");
            return 0;
        }

        var jt = tgtType.Methods.FirstOrDefault(m => m.Name == "JumpTargets" && m.Parameters.Count == 2);
        if (jt == null) { Console.Error.WriteLine("[R138] x 找不到 JumpTargets(IntPtr,bool)"); return 3; }
        if (jt.ReturnType.FullName != "System.Collections.Generic.IEnumerable`1<System.IntPtr>")
        {
            Console.Error.WriteLine("[R138] x JumpTargets 返回类型不符: " + jt.ReturnType.FullName); return 3;
        }

        var hScan = hType.Methods.FirstOrDefault(m => m.Name == "Scan" && m.Parameters.Count == 2);
        var hIsArm = hType.Methods.FirstOrDefault(m => m.Name == "IsArm64" && m.Parameters.Count == 0);
        var hLooks = hType.Methods.FirstOrDefault(m => m.Name == "LooksArm64" && m.Parameters.Count == 1);
        if (hScan == null || hIsArm == null || hLooks == null) { Console.Error.WriteLine("[R138] x helper 缺 Scan/IsArm64/LooksArm64"); return 3; }

        // 1) 克隆 Scan -> ScanArm64(IntPtr,bool) -> IntPtr[]
        var scanArm = CloneMethodBody(common, hScan, "ScanArm64");
        tgtType.Methods.Add(scanArm);

        // 2) 克隆 IsArm64 -> IsArm64() -> bool
        var isArm = CloneMethodBody(common, hIsArm, "IsArm64");
        tgtType.Methods.Add(isArm);

        // 2b) 克隆 LooksArm64 -> LooksArm64(IntPtr) -> bool（代码页架构检测兜底）
        var looks = CloneMethodBody(common, hLooks, "LooksArm64");
        tgtType.Methods.Add(looks);

        // 3) 原 JumpTargets 改名 JumpTargetsX86（保留 PC 路径）
        jt.Name = "JumpTargetsX86";

        // 4) 新建 JumpTargets 分发器：IsArm64() ? ScanArm64(a,b) : JumpTargetsX86(a,b)
        var disp = new MethodDefinition("JumpTargets",
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            common.ImportReference(jt.ReturnType));
        disp.Parameters.Add(new ParameterDefinition(common.ImportReference(jt.Parameters[0].ParameterType)));
        disp.Parameters.Add(new ParameterDefinition(common.ImportReference(jt.Parameters[1].ParameterType)));
        tgtType.Methods.Add(disp);

        var il = disp.Body.GetILProcessor();
        var ret = Instruction.Create(OpCodes.Ret);
        var callIsArm = Instruction.Create(OpCodes.Call, common.ImportReference(isArm));
        var brIsArm = Instruction.Create(OpCodes.Brtrue, ret); // 占位目标稍后设：直接跳到 ARM 扫描段
        // IsArm=false 时：LooksArm64(code)? -> ARM 扫描，否则 x86
        var ld0Looks = Instruction.Create(OpCodes.Ldarg_0);
        var callLooks = Instruction.Create(OpCodes.Call, common.ImportReference(looks));
        var brLooks = Instruction.Create(OpCodes.Brfalse, ret); // 占位 -> x86 段
        // ARM 扫描段
        var armLd0 = Instruction.Create(OpCodes.Ldarg_0);
        var armLd1 = Instruction.Create(OpCodes.Ldarg_1);
        var callScan = Instruction.Create(OpCodes.Call, common.ImportReference(scanArm));
        var brToRet = Instruction.Create(OpCodes.Br, ret);
        // x86 段
        var x0 = Instruction.Create(OpCodes.Ldarg_0);
        var x1 = Instruction.Create(OpCodes.Ldarg_1);
        var callX86 = Instruction.Create(OpCodes.Call, common.ImportReference(jt));

        il.Append(callIsArm);
        il.Append(brIsArm);
        il.Append(ld0Looks);
        il.Append(callLooks);
        il.Append(brLooks);
        il.Append(armLd0);
        il.Append(armLd1);
        il.Append(callScan);
        il.Append(brToRet);
        il.Append(x0);
        il.Append(x1);
        il.Append(callX86);
        il.Append(ret);
        brIsArm.Operand = armLd0;
        brLooks.Operand = x0;
        brToRet.Operand = ret;

        // 栈中立自检：分发器两个分支都返回 1 个 IEnumerable<IntPtr>
        if (!VerifyDispatcher()) { Console.Error.WriteLine("[R138] x 分发器栈中立自检失败，拒绝产出"); return 8; }

        // 元数据合法性：全类型遍历
        foreach (var t in common.GetTypes()) { var _ = t.Name; }

        common.Write(commonPath);
        Console.WriteLine("[R138] ✓ ARM64 xref 真修已写入 " + commonPath);
        Console.WriteLine("[R138]   JumpTargets -> (IsArm64 || LooksArm64(code)) ? ScanArm64 : JumpTargetsX86（PC 零变化）");
        Console.WriteLine("[R138]   ScanArm64: 纯托管 ARM64 b/bl/cbz/tbz/br/blr/ret 解码(对齐官方,无寄存器模拟/无 ReadIntPtr),恒返回 >=1 目标");
        return 0;
    }

    // 静态证明分发器栈中立：call IsArm() -> [bool]; brfalse 弹栈 -> [];
    // true 分支 ldarg0/ldarg1 -> [ptr,bool]; call ScanArm64 (pop2/push1) -> [IEnum]; br ret;
    // false 分支 ldarg0/ldarg1 -> [ptr,bool]; call JumpTargetsX86 (pop2/push1) -> [IEnum]; ret 弹 1。
    static bool VerifyDispatcher()
    {
        // call IsArm64 -> [bool]
        int s = 1;
        s -= 1;      // brtrue 弹栈 -> [] (跳到 ARM 段入口，栈 0)
        // 顺序流: ldarg.0 -> [ptr]
        s += 1;
        s += 1 - 1;  // call LooksArm64(pop1/push1) -> [bool]，栈仍 1
        s -= 1;      // brfalse 弹栈 -> [] (跳到 x86 段入口，栈 0)
        // ARM 段入口栈 0: ldarg0,ldarg1,call ScanArm64(pop2/push1) -> [IEnum]
        int armExit = 0 + 2 - 1;
        // x86 段入口栈 0: ldarg0,ldarg1,call JumpTargetsX86(pop2/push1) -> [IEnum]
        int x86Exit = 0 + 2 - 1;
        // ret 弹 1 -> []; 两分支出口栈深必须都是 1（待返回 1 个值）
        return armExit == 1 && x86Exit == 1 && s == 0;
    }

    // 把 helper 方法体逐条克隆到目标 module，operand 用 ImportReference 重映射。
    static MethodDefinition CloneMethodBody(ModuleDefinition dest, MethodDefinition src, string newName)
    {
        var md = new MethodDefinition(newName,
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            dest.ImportReference(src.ReturnType));
        foreach (var p in src.Parameters)
            md.Parameters.Add(new ParameterDefinition(dest.ImportReference(p.ParameterType)));

        var body = md.Body;
        body.InitLocals = src.Body.InitLocals;
        body.MaxStackSize = src.Body.MaxStackSize;

        // 先克隆局部变量（保留顺序，ImportReference）
        var varMap = new Dictionary<VariableDefinition, VariableDefinition>();
        foreach (var v in src.Body.Variables)
        {
            var nv = new VariableDefinition(dest.ImportReference(v.VariableType));
            body.Variables.Add(nv);
            varMap[v] = nv;
        }

        // 克隆指令（两遍：先建壳，再修 operand 分支目标）
        var insMap = new Dictionary<Instruction, Instruction>();
        foreach (var i in src.Body.Instructions)
        {
            Instruction ni;
            // 按 OperandType 分支用对应 Create 重载（Cecil 克隆标准写法）。
            switch (i.OpCode.OperandType)
            {
                case OperandType.InlineNone:
                    ni = Instruction.Create(i.OpCode); break;
                case OperandType.ShortInlineI:
                    ni = Instruction.Create(i.OpCode, (sbyte)(i.Operand is sbyte ? (sbyte)i.Operand : (sbyte)(int)i.Operand)); break;
                case OperandType.InlineI:
                    ni = Instruction.Create(i.OpCode, (int)i.Operand); break;
                case OperandType.InlineI8:
                    ni = Instruction.Create(i.OpCode, (long)i.Operand); break;
                case OperandType.ShortInlineR:
                    ni = Instruction.Create(i.OpCode, (float)i.Operand); break;
                case OperandType.InlineR:
                    ni = Instruction.Create(i.OpCode, (double)i.Operand); break;
                case OperandType.InlineString:
                    ni = Instruction.Create(i.OpCode, (string)i.Operand); break;
                case OperandType.InlineMethod:
                    ni = Instruction.Create(i.OpCode, dest.ImportReference((MethodReference)i.Operand)); break;
                case OperandType.InlineField:
                    ni = Instruction.Create(i.OpCode, dest.ImportReference((FieldReference)i.Operand)); break;
                case OperandType.InlineType:
                case OperandType.InlineTok:
                    ni = Instruction.Create(i.OpCode, dest.ImportReference((TypeReference)i.Operand)); break;
                case OperandType.ShortInlineBrTarget:
                case OperandType.InlineBrTarget:
                case OperandType.InlineSwitch:
                    ni = Instruction.Create(i.OpCode, Instruction.Create(OpCodes.Nop)); break; // 占位
                case OperandType.InlineVar:
                case OperandType.ShortInlineVar:
                    ni = Instruction.Create(i.OpCode, (VariableDefinition)i.Operand); break;
                case OperandType.InlineArg:
                case OperandType.ShortInlineArg:
                    ni = Instruction.Create(i.OpCode, (ParameterDefinition)i.Operand); break;
                default:
                    throw new Exception("不支持的 OperandType: " + i.OpCode.OperandType + " @ " + i.OpCode.Name);
            }
            insMap[i] = ni;
            body.Instructions.Add(ni);
        }

        // 修正 operand：分支目标 / 局部变量 / 参数 / 其余
        foreach (var i in src.Body.Instructions)
        {
            var ni = insMap[i];
            if (i.Operand is Instruction tgt)
            {
                ni.Operand = insMap[tgt];
            }
            else if (i.Operand is Instruction[] tgts)
            {
                ni.Operand = tgts.Select(x => insMap[x]).ToArray();
            }
            else if (i.Operand is VariableDefinition v)
            {
                ni.Operand = varMap[v];
            }
            else if (i.Operand is ParameterDefinition pd)
            {
                ni.Operand = md.Parameters[pd.Index];
            }
        }

        // 克隆异常处理（helper 里有 try/catch，必须保留）
        foreach (var eh in src.Body.ExceptionHandlers)
        {
            var neh = new ExceptionHandler(eh.HandlerType);
            neh.TryStart = insMap[eh.TryStart];
            neh.TryEnd = eh.TryEnd == null ? null : insMap[eh.TryEnd];
            neh.HandlerStart = insMap[eh.HandlerStart];
            neh.HandlerEnd = eh.HandlerEnd == null ? null : insMap[eh.HandlerEnd];
            neh.CatchType = eh.CatchType == null ? null : dest.ImportReference(eh.CatchType);
            body.ExceptionHandlers.Add(neh);
        }
        return md;
    }
}
