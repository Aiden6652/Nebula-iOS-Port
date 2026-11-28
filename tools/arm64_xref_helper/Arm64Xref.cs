using System;
using System.Runtime.InteropServices;

// R138: 严格对齐官方 Il2CppInterop v1.5.3 JumpTargetsImpl 源码语义（已从官方仓库取到源码 + Cecil 状态机 IL 双证）。
// 官方源码（XrefScannerLowLevel.cs）：
//   var firstFlowControl = true;
//   while(true) {
//     Decode(out instr); if LastError==NoMoreBytes break;
//     if Mnemonic==Int3 break;
//     if FlowControl==Return && !ignoreRetn break;
//     if FlowControl==UnconditionalBranch || FlowControl==Call:
//        if !IsJmpShort: yield ExtractTargetAddress(instr);
//                        if firstFlowControl && FlowControl==UnconditionalBranch: yield break;
//     if FlowControl != Next: firstFlowControl=false;
//   }
//   ExtractTargetAddress 只对 NearBranch16/32/64、FarBranch16/32 取 target（纯立即数），
//   对 IndirectBranch(寄存器跳) 会抛 ArgumentOutOfRangeException —— 官方根本不 yield 寄存器跳。
//
// ARM64 -> Iced FlowControl 映射（Iced 按指令类别分类，与助记符无关）：
//   b (imm26)       -> UnconditionalBranch : yield target; 若 firstFlowControl 则停
//   bl (imm26)      -> Call               : yield target; 不停（R137 漏 yield 这条，GenericMethod::GetMethod 靠它定位）
//   cbz/cbnz/tbz/tbnz -> ConditionalBranch: 不 yield; firstFlowControl=false
//   br/blr (寄存器)  -> IndirectBranch     : 不 yield（官方 ExtractTargetAddress 对寄存器跳会抛异常）; firstFlowControl=false
//   ret             -> Return            : break
//   其它(adrp/add/ldr/nop..)-> Next       : 不 yield; 不改 firstFlowControl
//
// R137 的两个致命错误（真机 BUILD=60f6e19b18fd 暴露：found=8 含重复值/跨段野值 0x115E13A40，紧接 SIGBUS）：
//   (1) 用 IntPtr[] regs 模拟 ADRP/ADD/LDR 读 GOT，再对 BR/BLR yield regs[rn] —— 读野地址触发
//       Marshal.ReadIntPtr 的 native SIGBUS（try-catch 抓不住），且野值被 detour 当 target 打错地址；
//   (2) BL(Call) 不 yield —— GenericMethod::GetMethod 等靠 call 目标定位的 hook 拿不到 target。
// R138 删除全部寄存器模拟（官方本就不做），只保留 near-branch 立即数解码；无内存读、无 SIGBUS 风险。
// 纯托管（位运算），不依赖 Iced/Capstone 原生库；方法自包含便于 Cecil 克隆。
namespace NebulaArm64Xref
{
    public static class Arm64Xref
    {
        const int SCAN_BYTES = 1000;

        public static bool IsArm64()
        {
            return RuntimeInformation.ProcessArchitecture == Architecture.Arm64;
        }

        // 代码页上判定架构，不依赖 iOS 上可能返回 Unknown 的 RuntimeInformation.ProcessArchitecture。
        public static bool LooksArm64(IntPtr code)
        {
            int hits = 0;
            try
            {
                for (int off = 0; off + 4 <= 64; off += 4)
                {
                    uint w = (uint)Marshal.ReadInt32(code, off);
                    if (w == 0u) break;
                    bool arm =
                        ((w & 0x7C000000u) == 0x14000000u) ||
                        ((w & 0x7E000000u) == 0x34000000u) ||
                        ((w & 0x7E000000u) == 0x36000000u) ||
                        ((w & 0x9F000000u) == 0x90000000u) ||
                        ((w & 0xFFC00000u) == 0xF9400000u) ||
                        ((w & 0x7F800000u) == 0x11000000u) ||
                        ((w & 0xFFFFFC1Fu) == 0xD65F0000u) ||
                        ((w & 0xFFFFFC1Fu) == 0xD61F0000u);
                    if (arm) { hits++; if (hits >= 2) break; }
                }
            }
            catch { }
            return hits >= 2;
        }

        // 返回 code 处 ARM64 thunk 的 near-branch 跳转目标列表（严格对齐官方 JumpTargetsImpl）。
        // 恒返回 >=1 个（解码失败回退 code 本身），保证上层 Single()/First()/Last()/ElementAt() 不崩。
        // 无内存读（只读 code 页指令字本身），不会触发 native SIGBUS。
        public static IntPtr[] Scan(IntPtr code, bool useXref)
        {
            IntPtr[] targets = new IntPtr[64];
            int n = 0;
            long codeLong = code.ToInt64();
            bool firstFlowControl = true;
            try
            {
                for (int off = 0; off + 4 <= SCAN_BYTES; off += 4)
                {
                    uint w = (uint)Marshal.ReadInt32(code, off);
                    long pc = codeLong + off;
                    if (w == 0u) break;

                    // B (0x14000000, imm26): FlowControl.UnconditionalBranch
                    //   -> yield target; 若 firstFlowControl 则 yield break（官方首条无条件跳即停）。
                    if ((w & 0xFC000000u) == 0x14000000u)
                    {
                        long o = (long)(w & 0x03FFFFFFu) << 2;
                        if ((w & 0x02000000u) != 0u) o |= ~0x0FFFFFFF;
                        long t = pc + o;
                        if (t != 0 && n < 64) targets[n++] = (IntPtr)t;
                        if (firstFlowControl) break;
                        continue;
                    }

                    // BL (0x94000000, imm26): FlowControl.Call
                    //   -> yield target; 不停（只有 UnconditionalBranch 才触发首条停）。
                    if ((w & 0xFC000000u) == 0x94000000u)
                    {
                        long o = (long)(w & 0x03FFFFFFu) << 2;
                        if ((w & 0x02000000u) != 0u) o |= ~0x0FFFFFFF;
                        long t = pc + o;
                        if (t != 0 && n < 64) targets[n++] = (IntPtr)t;
                        firstFlowControl = false;
                        continue;
                    }

                    // CBZ / CBNZ / TBZ / TBNZ: FlowControl.ConditionalBranch -> 不 yield。
                    if ((w & 0x7E000000u) == 0x34000000u || (w & 0x7E000000u) == 0x35000000u ||
                        (w & 0x7E000000u) == 0x36000000u || (w & 0x7E000000u) == 0x37000000u)
                    {
                        firstFlowControl = false;
                        continue;
                    }

                    // BR / BLR (0xD61F0000 / 0xD63F0000, 寄存器间接跳): FlowControl.IndirectBranch
                    //   -> 官方不 yield（ExtractTargetAddress 对寄存器跳会抛异常），只令 firstFlowControl=false。
                    //   R137 在这里 yield regs[rn] 野值 -> detour 打错地址 -> SIGBUS，R138 彻底删除。
                    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u)
                    {
                        firstFlowControl = false;
                        continue;
                    }

                    // RET: FlowControl.Return -> break
                    if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) break;

                    // 其它指令（adrp/add/ldr/subs/mov/nop...）= FlowControl.Next:
                    //   不 yield，不改 firstFlowControl（官方语义）。无内存读。
                }
            }
            catch { }

            if (n == 0)
            {
                targets[0] = code;
                n = 1;
            }

            IntPtr[] result = new IntPtr[n];
            for (int i = 0; i < n; i++) result[i] = targets[i];

            try
            {
                string line = "[R138-XREF] code=0x" + code.ToInt64().ToString("X") + " found=" + n.ToString();
                for (int i = 0; i < n; i++)
                    line += " t" + i.ToString() + "=0x" + result[i].ToInt64().ToString("X");
                Console.WriteLine(line);
            }
            catch { }
            return result;
        }
    }
}
