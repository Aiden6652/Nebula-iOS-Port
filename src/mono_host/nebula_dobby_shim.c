/*
 * [R61] BepInEx6 IL2CPP 原生 detour 桥接 shim
 * --------------------------------------------------
 * BepInEx6 的 BepInEx.Unity.IL2CPP.Hook.Dobby.DobbyDetour 走「两段式」API：
 *     DobbyPrepare(target, replace, &prepared) -> 分配 trampoline（prepared = 原函数指针）
 *     DobbyCommit(prepared)                    -> 真正把 detour 写入内存
 *     DobbyDestroy(address)                    -> 撤销
 * 但本仓库静态链的是上游 jmpews/dobby，它只有一次性 DobbyHook（prepare+commit 一起完成），
 * 根本没有 DobbyPrepare / DobbyCommit 这两个符号。于是 IL2CPPChainloader.Initialize
 * 在 DobbyDetour.PrepareImpl() 里 P/Invoke "DobbyPrepare" 时抛：
 *     System.EntryPointNotFoundException: DobbyPrepare
 * BepInEx 随即整个退出，游戏直接进原版、NOS 不加载（真机 preloader.log / nebula_host.txt 实锤）。
 *
 * 宿主之前只探测 DobbyHook 是否存在就判定 dobby「已就绪」并跳过 R56 兜底，结果官方 detour
 * 路径在 DobbyPrepare 这步崩。这里用 DobbyHook 包出 DobbyPrepare / DobbyCommit，让官方
 * detour 路径能正常装上，NOS 走正路加载。
 *
 * 语义对齐（BepInEx 调用顺序 = Prepare 紧跟 Commit，不会在中间调用 target）：
 *   - DobbyPrepare：直接 DobbyHook，hook 在此时写好；prepared 填 trampoline（调原函数用）。
 *   - DobbyCommit ：hook 已在 Prepare 写完，这里为空操作，返回 0（成功）。
 *   - DobbyDestroy：上游 dobby 已有，不再重复定义。
 *
 * 注：宿主本身用 dlsym 取 DobbyHook，编译期不 include dobby.h，故这里前向声明即可，
 * 不依赖头文件搜索路径。
 *
 * ============================================================================
 * [R95e] 新增：dobby detour 诊断 + 安全降级（修 NOS Harmony.PatchAll 的 native SIGSEGV）
 * ----------------------------------------------------------------------------
 * 真机（R95d, BUILD=1e3e96df3587）实证：R95c/R95d 的 Cecil resolver 修复已生效，
 * MissingMethodException / IL Compile Error 全部消失，5 插件加载、NOS 本体进
 * NebulaPlugin.LoadInternal -> Harmony.PatchAll，随后崩在 native：
 *     Il2CppDetourMethodPatcher.DetourTo -> Il2CppInteropDetour.Apply
 *     -> DobbyDetour.PrepareImpl -> DobbyLib.Prepare -> DobbyHook
 *     -> GenerateRelocatedCode -> GenRelocateCodeAndBranch -> relo_relocate  SIGSEGV
 * 这是 ARM64 inline-hook 重定位阶段，跟托管 IL 层无关（全新的一堵墙）。
 *
 * 本 shim 是 DobbyHook 之前唯一的自有 C 代码，在这里：
 *   1) 每次 hook 打印序号 + target/replace 地址（stdout 已被宿主 dup2 到日志管道）；
 *   2) 用 vm_read 安全读取 target 开头 16 字节并 hexdump —— 不直接 deref，
 *      避免「诊断代码自己先崩」；读不到 = 非法地址（NULL / 野指针 / PAC 未清）；
 *   3) 非法地址 -> 不调 DobbyHook，把 *prepared 填成原函数地址（patch 静默失效
 *      但绝不崩）。关键：DobbyLib::Prepare 的返回码被 IL 的 pop 丢弃，且 DetourTo
 *      在 IL_00d2 把 *prepared 写进 modifiedNativeMethodInfo.MethodPointer，
 *      故必须正确填 trampoline；留 0 会导致之后调用跳 NULL 崩（比当场崩更隐蔽）；
 *   4) 疑似 arm64e PAC 签名（高 16 位非零）：仅当原始不可读、清高位后可读时才用
 *      清理值真 hook（这是「修复」），并打印；合法地址高位为 0，绝不误伤；
 *   5) 正常地址照常 DobbyHook；即使 DobbyHook 返回失败也兜底填 *prepared。
 *
 * 结果分两种，都不亏：
 *   - 真因是「某个 patch 的目标地址非法/带 PAC」-> 被拦下或修好，NOS 其余 patch
 *     照常装，可能这版就直接跑起来；
 *   - 真因是「arm64e 指令老 dobby 重定位器不支持」-> 地址可读仍会崩，但 hexdump
 *     与最后一行日志直接给出确切指令，下一版精准修，不用再猜。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>              /* getpagesize */
#include <dlfcn.h>               /* dladdr：判定目标地址是否属于已加载 Mach-O image */
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>         /* vm_read：仅用于 hexdump 取证（宿主 R62/R63 已验证此头在 iOS SDK 可用） */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 上游 jmpews/dobby 的真实签名（dobby_dummy_func_t == void*）。 */
int DobbyHook(void *address, void *replace_func, void **origin_func);

#ifdef __cplusplus
}
#endif

/* 页对齐读 addr 前 n 字节做 hexdump 取证（纯诊断，不参与判定）。
   vm_read 对非法地址返回 kern 错误而非 SIGSEGV；显式按页起始读、再按 off 取，
   规避不同 Darwin 版本「返回缓冲区是否含页内偏移」的语义歧义。
   非 __APPLE__ 分支仅用于本地 gcc 语法检查，不参与 iOS 构建。 */
static int r95e_hexdump(void *addr, unsigned char *out, unsigned n) {
#ifdef __APPLE__
    long ps = getpagesize();
    if (ps <= 0) return 0;
    vm_address_t a = (vm_address_t)addr;
    vm_address_t page = a & ~(vm_address_t)(ps - 1);
    vm_size_t off = (vm_size_t)(a - page);
    vm_offset_t data = 0;
    mach_msg_type_number_t cnt = 0;
    kern_return_t kr = vm_read(mach_task_self(), page,
                               (vm_size_t)(off + n), &data, &cnt);
    if (kr != KERN_SUCCESS || cnt < (mach_msg_type_number_t)(off + n)) {
        if (data) vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
        return 0;
    }
    memcpy(out, (const void *)(uintptr_t)(data + off), n);
    vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
    return 1;
#else
    memcpy(out, addr, n);
    return 1;
#endif
}

/* iOS arm64e 用户态虚拟地址低 48 位有效，高 16 位可能是 PAC 签名 / TBI tag。 */
static void *r95e_strip_pac(void *p) {
    return (void *)(uintptr_t)((unsigned long long)(uintptr_t)p & 0x0000FFFFFFFFFFFFull);
}

/* 判定目标地址是否属于某个已加载 Mach-O image。IL2CPP 原生方法指针必然落在
   UnityFramework / 主二进制的 __TEXT 里；返回 0 = 野指针 / stub / 未映射地址，
   dobby 拿去 relo_relocate 必崩。dladdr 是标准 POSIX/Darwin API，只读不触碰内存。 */
static int r95e_in_image(void *p, const char **fname) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    int ok = dladdr(p, &info) != 0 && info.dli_fbase != NULL;
    *fname = ok && info.dli_fname ? info.dli_fname : "(none)";
    return ok;
}

__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    static unsigned seq = 0;
    seq++;

    if (seq == 1)
        printf("[host][r95e] ===== R95e：dobby detour 诊断+安全降级版（逐 patch 记目标地址/归属 image/首指令，坏地址不崩）=====\n");

    unsigned long long t = (unsigned long long)(uintptr_t)target;
    unsigned long long r = (unsigned long long)(uintptr_t)replace;

    /* 绝对兜底：trampoline = 原函数（不 patch 也不崩）。
       DetourTo 在 IL_00d2 会把 *prepared 写进 modifiedNativeMethodInfo.MethodPointer，
       留 0 会导致之后调用跳 NULL 崩；DobbyHook 成功时会覆写成真 trampoline。 */
    if (prepared) *prepared = target;

    /* —— 修复①：arm64e PAC / TBI 高位签名，清掉高 16 位还原真实代码地址 —— */
    void *hook_addr = target;
    unsigned long long hi = (unsigned long long)((uintptr_t)t >> 48);
    if (hi != 0) {
        void *cleaned = r95e_strip_pac(target);
        const char *cn = "(none)";
        int cok = r95e_in_image(cleaned, &cn);
        printf("[r95e][dob] #%u 高位=0x%llx 疑似 arm64e PAC/TBI 签名 -> 清理后 0x%llx in_image=%d %s\n",
               seq, hi, (unsigned long long)(uintptr_t)cleaned, cok, cn);
        fflush(stdout);
        if (cok) { hook_addr = cleaned; if (prepared) *prepared = cleaned; }
    }

    /* —— 硬判定（dladdr + 算术）：铁定非法才降级，可疑但合法一律放行 —— */
    const char *fname = "(none)";
    int in_image = r95e_in_image(hook_addr, &fname);
    int nullish  = ((uintptr_t)hook_addr < 0x100000000ull);   /* NULL / 低 4GB 非法区 */
    int misalign = (((uintptr_t)hook_addr & 3ull) != 0);      /* ARM64 指令须 4 字节对齐 */

    printf("[r95e][dob] #%u target=0x%llx replace=0x%llx in_image=%d %s nullish=%d misalign=%d\n",
           seq, (unsigned long long)(uintptr_t)hook_addr, r, in_image, fname, nullish, misalign);
    fflush(stdout);

    /* —— 修复②：野指针 / stub / 未映射目标，跳过该 hook 而不是让 relo_relocate 崩 —— */
    if (!in_image || nullish || misalign) {
        printf("[r95e][dob] #%u 降级跳过：*prepared=原函数，该 patch 静默失效，进程不崩\n", seq);
        fflush(stdout);
        return 0;   /* 返回码被 IL pop 丢弃；*prepared 已填好 */
    }

    /* —— 诊断：hexdump 前 16 字节。SIGSEGV 前最后一行即罪魁，
       首指令是不是 paciasp(0xd50323bf)/bti 等 arm64e 指令一目了然 —— */
    unsigned char head[16];
    memset(head, 0, sizeof(head));
    int got_head = r95e_hexdump(hook_addr, head, sizeof(head));
    if (got_head) {
        printf("[r95e][dob] #%u head=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
               seq, head[0],  head[1],  head[2],  head[3],
                     head[4],  head[5],  head[6],  head[7],
                     head[8],  head[9],  head[10], head[11],
                     head[12], head[13], head[14], head[15]);
    } else {
        printf("[r95e][dob] #%u head= <vm_read 失败，地址不可读>\n", seq);
    }
    fflush(stdout);

    /* —— 修复③：首指令是 arm64e PAC/BTI 指令时跳过该 hook ——
       上游 jmpews/dobby 的 InstructionRelocationARM64 不认识这些编码，
       relo_relocate 会走错误分支 SIGSEGV。
       判据用【精确白名单】而非范围匹配：范围判据会误伤 NOP(0xd503201f)/
       YIELD/WFE 等合法 HINT 指令，导致本可成功的 hook 被跳过（比漏判更难查）。
       宁可漏判（日志留 hexdump，下一版补），绝不误伤。 —— */
    if (got_head) {
        uint32_t insn = (uint32_t)head[0] | ((uint32_t)head[1] << 8) |
                        ((uint32_t)head[2] << 16) | ((uint32_t)head[3] << 24);
        static const uint32_t kArm64ePrologue[] = {
            0xD50323BFu, /* PACIASP */ 0xD503233Fu, /* PACIASP (alt encoding) */
            0xD50327BFu, /* PACIBSP */ 0xD503273Fu, /* PACIBSP (alt) */
            0xD50323FFu, /* AUTIASP */ 0xD50327FFu, /* AUTIBSP */
            0xD503211Fu, /* XPACLRI */
            0xD503241Fu, /* BTI    */ 0xD503245Fu, /* BTI c */
            0xD503249Fu, /* BTI j  */ 0xD50324DFu, /* BTI jc */
        };
        int hit = 0;
        for (unsigned k = 0; k < sizeof(kArm64ePrologue)/sizeof(kArm64ePrologue[0]); k++)
            if (insn == kArm64ePrologue[k]) { hit = 1; break; }
        if (hit) {
            printf("[r95e][dob] #%u 降级跳过：首指令 0x%08x 是 arm64e PAC/BTI 序言，"
                   "老 dobby 重定位器不支持（*prepared=原函数，进程不崩）\n", seq, insn);
            fflush(stdout);
            return 0;
        }
        /* 其它 HINT 只诊断、不拦截（放宽疑似的取证，避免误伤） */
        if (head[3] == 0xD5 && head[2] == 0x03)
            printf("[r95e][dob] #%u 注意：首指令 0x%08x 属 HINT 族但未命中 PAC/BTI 白名单，照常 hook\n",
                   seq, insn);
    }

    int rc = DobbyHook(hook_addr, replace, prepared);
    if (rc != 0) {
        printf("[r95e][dob] #%u DobbyHook rc=%d（失败）-> 兜底 *prepared=原函数\n", seq, rc);
        fflush(stdout);
        if (prepared) *prepared = hook_addr;
    }
    return rc;
}

__attribute__((visibility("default")))
int DobbyCommit(void *prepared) {
    (void)prepared;
    return 0;
}
