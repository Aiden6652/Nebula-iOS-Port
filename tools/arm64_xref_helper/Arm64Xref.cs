using System;
using System.Runtime.InteropServices;

// R137: 精确复刻 Il2CppInterop 官方 JumpTargetsImpl 状态机语义（从 Cecil IL 反编译还原）：
//   firstFlowControl=true; 逐条解码：
//     FlowControl.UnconditionalBranch(b)   -> yield target; 若 firstFlowControl 则停止
//     FlowControl.IndirectBranch(br/blr)   -> yield target; 继续
//     FlowControl.Call(bl)                 -> 不 yield; 继续
//     FlowControl.ConditionalBranch(cbz/tbz) -> 不 yield; 继续
//     FlowControl.Return(ret)              -> 停止
//   非 Next 指令令 firstFlowControl=false。
// R136 的错误：把所有跳转都 yield 且只返回 targets[n-1] 砍成 1 个，
//   既破坏了上层 Count()==1 判断（致 ElementAt(1)/Single 语义错乱），target 也取错（取 last 而非第一条）。
// 本实现返回真实目标列表（至少 1 个，解码失败回退 code），让上层 Single/Last/First/ElementAt/Count 各取所需。
// 纯托管 ARM64 解码（Marshal.ReadInt32 + 位运算），不依赖 Iced/Capstone 原生库；方法自包含便于 Cecil 克隆。
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

        // 返回 code 处 ARM64 thunk 的跳转目标列表（对齐官方 JumpTargetsImpl 语义）。
        // 恒返回 >=1 个（解码失败回退 code 本身），保证上层 Single()/First()/Last() 不崩。
        public static IntPtr[] Scan(IntPtr code, bool useXref)
        {
            IntPtr[] targets = new IntPtr[64];
            int n = 0;
            IntPtr[] regs = new IntPtr[31]; // x0..x30
            long codeLong = code.ToInt64();
            bool firstFlowControl = true;
            try
            {
                for (int off = 0; off + 4 <= SCAN_BYTES; off += 4)
                {
                    uint w = (uint)Marshal.ReadInt32(code, off);
                    long pc = codeLong + off;
                    if (w == 0u) break;

                    // B (0x14000000, 无条件跳转): yield target; firstFlowControl 时立即停止。
                    // 精确匹配官方：第一条 UnconditionalBranch yield 后 yield break。
                    if ((w & 0xFC000000u) == 0x14000000u)
                    {
                        long o = (long)(w & 0x03FFFFFFu) << 2;
                        if ((w & 0x02000000u) != 0u) o |= ~0x0FFFFFFF;
                        long t = pc + o;
                        if (t != 0 && n < 64) targets[n++] = (IntPtr)t;
                        if (firstFlowControl) break;
                        continue;
                    }

                    // BL (0x94000000, call): 官方 FlowControl.Call 不 yield；令 firstFlowControl=false。
                    if ((w & 0xFC000000u) == 0x94000000u)
                    {
                        firstFlowControl = false;
                        continue;
                    }

                    // CBZ / CBNZ / TBZ / TBNZ (条件跳转): 官方 ConditionalBranch 不 yield。
                    if ((w & 0x7E000000u) == 0x34000000u || (w & 0x7E000000u) == 0x35000000u ||
                        (w & 0x7E000000u) == 0x36000000u || (w & 0x7E000000u) == 0x37000000u)
                    {
                        firstFlowControl = false;
                        continue;
                    }

                    // ADRP: 计算页基址 -> regs[rd]
                    if ((w & 0x9F000000u) == 0x90000000u)
                    {
                        int rd = (int)(w & 0x1Fu);
                        ulong immlo = (w >> 29) & 3u;
                        ulong immhi = (w >> 5) & 0x7FFFFu;
                        long pageOff = (long)((immhi << 2) | immlo) << 12;
                        if ((((immhi << 2) | immlo) & 0x100000u) != 0u) pageOff |= ~0x1FFFFFFFF;
                        long page = (pc & ~0xFFF) + pageOff;
                        if (rd < 31) regs[rd] = (IntPtr)page;
                        continue;
                    }

                    // ADD immediate (64-bit): regs[rd] = regs[rn] + imm
                    if ((w & 0x7F800000u) == 0x11000000u && ((w >> 31) & 1u) == 1u)
                    {
                        int rd = (int)(w & 0x1Fu);
                        int rn = (int)((w >> 5) & 0x1Fu);
                        int shift = ((w >> 22) & 1u) == 1u ? 12 : 0;
                        long imm = (long)(((w >> 10) & 0xFFFu) << shift);
                        if (rd < 31 && rn < 31 && regs[rn] != IntPtr.Zero)
                            regs[rd] = (IntPtr)(regs[rn].ToInt64() + imm);
                        continue;
                    }

                    // LDR immediate unsigned (64-bit): regs[rt] = [regs[rn] + imm*8]
                    if ((w & 0xFFC00000u) == 0xF9400000u)
                    {
                        int rt = (int)(w & 0x1Fu);
                        int rn = (int)((w >> 5) & 0x1Fu);
                        long imm = (long)(((w >> 10) & 0xFFFu) * 8u);
                        if (rt < 31 && rn < 31 && regs[rn] != IntPtr.Zero)
                        {
                            try { regs[rt] = Marshal.ReadIntPtr((IntPtr)(regs[rn].ToInt64() + imm)); }
                            catch { }
                        }
                        continue;
                    }

                    // BR / BLR (0xD61F0000 / 0xD63F0000, 间接跳转): yield regs[rn], 继续。
                    // 官方 FlowControl.IndirectBranch yield 后继续扫描（令 firstFlowControl=false）。
                    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u)
                    {
                        int rn = (int)((w >> 5) & 0x1Fu);
                        if (rn < 31 && regs[rn] != IntPtr.Zero)
                        {
                            long t = regs[rn].ToInt64();
                            if (t != 0 && n < 64) targets[n++] = (IntPtr)t;
                        }
                        firstFlowControl = false;
                        continue;
                    }

                    // RET: 官方 FlowControl.Return -> 停止
                    if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) break;
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
                string line = "[R137-XREF] code=0x" + code.ToInt64().ToString("X") + " found=" + n.ToString();
                for (int i = 0; i < n; i++)
                    line += " t" + i.ToString() + "=0x" + result[i].ToInt64().ToString("X");
                Console.WriteLine(line);
            }
            catch { }
            return result;
        }
    }
}
