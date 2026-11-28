using System;
using System.Runtime.InteropServices;

// R136: Il2CppInterop 1.5.3 的 XrefScannerLowLevel.JumpTargets 用 Iced x86 decoder，
// 在 ARM64 上解不出 b/bl 目标 -> 返回空 -> 上层 Single() 抛
// "Sequence contains no elements" -> RegisterTypeInIl2Cpp (class injection) 全崩。
// 本类提供纯托管 ARM64 指令扫描（Marshal.ReadInt32 读代码页 + 位运算），不依赖
// Iced / Capstone 原生库。所有方法自包含（符号扩展内联），便于 Cecil 方法体克隆进
// Il2CppInterop.Common.dll；只引用 BCL，ImportReference 可自动重映射。
namespace NebulaArm64Xref
{
    public static class Arm64Xref
    {
        // IL2CPP 导出 thunk 一般只有几条指令；限定 1000 字节（与原 Iced DecoderForAddress 一致）。
        const int SCAN_BYTES = 1000;

        // 架构判断：patched JumpTargets 分发器据此选 ARM64 扫描 / 原 x86 路径。
        // PC/x86 上返回 false -> 走原路径，零变化。
        public static bool IsArm64()
        {
            return RuntimeInformation.ProcessArchitecture == Architecture.Arm64;
        }

        // 直接在代码页上判定架构，不依赖 RuntimeInformation.ProcessArchitecture
        // （iOS 上该 API 可能返回 Unknown）。ARM64 b/bl/cbz/tbz/adrp/ret 的位模式在
        // x86-64 上是非法或极罕见 opcode；连续命中 2 条即可高置信判定 ARM64。
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
                        ((w & 0x7C000000u) == 0x14000000u) || // b/bl
                        ((w & 0x7E000000u) == 0x34000000u) || // cbz/cbnz
                        ((w & 0x7E000000u) == 0x36000000u) || // tbz/tbnz
                        ((w & 0x9F000000u) == 0x90000000u) || // adrp
                        ((w & 0xFFC00000u) == 0xF9400000u) || // ldr imm
                        ((w & 0x7F800000u) == 0x11000000u) || // add/sub imm
                        ((w & 0xFFFFFC1Fu) == 0xD65F0000u) || // ret
                        ((w & 0xFFFFFC1Fu) == 0xD61F0000u);   // br
                    if (arm) { hits++; if (hits >= 2) break; }
                }
            }
            catch { }
            return hits >= 2;
        }

        // 从 code 开始扫描最多 1000 字节，解析 b/bl/cbz/cbnz/tbz/tbnz/adrp/add/ldr/br/blr/ret，
        // 返回跳转目标（恒至少 1 个：解码失败回退 code 本身，保证上层 Single()/Last() 不崩）。
        // useXref 保留原签名（本实现忽略，ARM64 下统一做跳转目标解析）。
        public static IntPtr[] Scan(IntPtr code, bool useXref)
        {
            IntPtr[] targets = new IntPtr[64];
            int n = 0;
            IntPtr[] regs = new IntPtr[31]; // x0..x30，0 表示未知
            long codeLong = code.ToInt64();
            try
            {
                for (int off = 0; off + 4 <= SCAN_BYTES; off += 4)
                {
                    uint w = (uint)Marshal.ReadInt32(code, off);
                    long pc = codeLong + off;
                    if (w == 0u) break;

                    // B / BL: 0x14000000 / 0x94000000, imm26 -> target = pc + sext(imm26<<2, 28)
                    if ((w & 0x7C000000u) == 0x14000000u || (w & 0x7C000000u) == 0x94000000u)
                    {
                        long o = (long)(w & 0x03FFFFFFu) << 2;
                        if ((w & 0x02000000u) != 0u) o |= ~0x0FFFFFFF;
                        long t = pc + o;
                        if (t != 0) targets[n++] = (IntPtr)t;
                        continue;
                    }

                    // CBZ / CBNZ: 0x34000000 / 0x35000000, imm19 -> pc + sext(imm19<<2, 21)
                    if ((w & 0x7E000000u) == 0x34000000u || (w & 0x7E000000u) == 0x35000000u)
                    {
                        long o = (long)((w >> 5) & 0x7FFFFu) << 2;
                        if (((w >> 5) & 0x80000u) != 0u) o |= ~0x1FFFFF;
                        long t = pc + o;
                        if (t != 0) targets[n++] = (IntPtr)t;
                        continue;
                    }

                    // TBZ / TBNZ: 0x36000000 / 0x37000000, imm14 -> pc + sext(imm14<<2, 16)
                    if ((w & 0x7E000000u) == 0x36000000u || (w & 0x7E000000u) == 0x37000000u)
                    {
                        long o = (long)((w >> 5) & 0x3FFFu) << 2;
                        if (((w >> 5) & 0x2000u) != 0u) o |= ~0xFFFF;
                        long t = pc + o;
                        if (t != 0) targets[n++] = (IntPtr)t;
                        continue;
                    }

                    // ADRP: 0x90000000, page = (pc & ~0xFFF) + sext(immhi:immlo, 21) << 12
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

                    // ADD immediate (64-bit, sf=1): 0x91000000, reg[Rd] = reg[Rn] + imm12<<shift
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

                    // LDR immediate unsigned (64-bit): 0xF9400000, reg[Rt] = [reg[Rn] + imm12*8]
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

                    // BR / BLR: 0xD61F0000 / 0xD63F0000, target = reg[Rn]
                    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u)
                    {
                        int rn = (int)((w >> 5) & 0x1Fu);
                        if (rn < 31 && regs[rn] != IntPtr.Zero)
                        {
                            long t = regs[rn].ToInt64();
                            if (t != 0) targets[n++] = (IntPtr)t;
                        }
                        break;
                    }

                    // RET: 终止扫描
                    if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) break;
                }
            }
            catch { }

            IntPtr[] result = new IntPtr[1];
            if (n > 0)
                result[0] = targets[n - 1]; // 优先最后一个外部跳转目标（thunk 尾跳）
            else
                result[0] = code; // 解码失败回退导出地址，保证 Single() 恒得 1 个

            try
            {
                Console.WriteLine("[R136-XREF] code=0x" + code.ToInt64().ToString("X") +
                                  " found=" + n.ToString() + " target=0x" + result[0].ToInt64().ToString("X"));
            }
            catch { }
            return result;
        }
    }
}
