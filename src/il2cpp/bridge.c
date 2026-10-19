/*
 * bridge.c — 无 Frida、无越狱的 IL2CPP 桥（dylib 自包含）
 *
 * 只在【同一个进程内】解析 libil2cpp 导出 API，读/写游戏对象字段，
 * 并可用 il2cpp_runtime_invoke 调原版方法。不 patch 代码、不写可执行内存。
 *
 * 可观测性：全部通过 syslog 输出（<syslog.h>）。电脑用 macOS「控制台」或
 * idevicesyslog 即可看到，**无需越狱、无需读手机文件**。
 */

#include "bridge.h"
#include "hook.h"

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define NEBULA_LOG_TAG "Nebula-iOS"

/* Il2CppArray 布局（64 位）：obj(16) + bounds(8) + max_length(4) + pad → 元素从 0x20 起 */
#define ARR_LEN_OFF   0x18
#define ARR_DATA_OFF  0x20

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------- 日志：syslog + 文件（文件可被 iTunes 文件共享取出，不需越狱） ---------- */
void nebula_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);

    openlog(NEBULA_LOG_TAG, LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_NOTICE, "%s", buf);

    const char *home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/Documents/nebula_log.txt", home);
        FILE *fp = fopen(path, "a");
        if (fp) { fprintf(fp, "[Nebula-iOS] %s\n", buf); fclose(fp); }
    }
}

/* ---------- 符号解析：dlsym 失败时，直接扫 Mach-O 静态符号表 ----------
 * dlsym 只看【动态导出表】；新版本 Among Us 很可能把 il2cpp_* 从那里移除，
 * 但静态符号仍在符号表里。这里直接解析 Mach-O 的 LC_SYMTAB 兜底。 */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

/* ★ 关键修正：Mach-O 的【文件偏移】不等于【内存地址偏移】。
 *
 * 真实案例（用户 IPA 里的 UnityFramework）：
 *   __DATA     vm=0x5590000 vmsize=0x864000  file=0x5590000 filesize=0x4c8000
 *              ↑ vm 比 file 大（有零填充段）
 *   __LINKEDIT vm=0x5df4000                  file=0x5a58000
 *              ↑ 两者相差 0x29C000
 *   LC_SYMTAB 的 symoff=0x5bbd260 是【文件偏移】
 *
 * 若直接用 "基址 + symoff" 会偏移 0x29C000，读到 __DATA 中间去，
 * 再按 10957 个符号遍历 + 用错误字符串表索引 → 必然闪退。
 * 正确做法：找到该偏移所属的 segment，用它的 (vmaddr, fileoff) 做换算：
 *      地址 = 基址 + seg.vmaddr + (文件偏移 - seg.fileoff)
 */
typedef struct { uint64_t vmaddr, vmsize, fileoff, filesize; } seg_t;
#define MAX_SEGS 8

/* 把文件偏移换算为镜像内相对地址 */
static int file_off_to_addr(seg_t *segs, int n, uint64_t foff, uint64_t need, uint64_t *out)
{
    for (int i = 0; i < n; i++) {
        if (segs[i].filesize == 0) continue;
        if (foff >= segs[i].fileoff && foff + need <= segs[i].fileoff + segs[i].filesize) {
            *out = segs[i].vmaddr + (foff - segs[i].fileoff);
            return 1;
        }
    }
    return 0;
}

/* 收集段信息，用于符号表越界校验与地址换算 */
static int collect_segments(const struct mach_header *mh, seg_t *segs, int max)
{
    uintptr_t base = (uintptr_t)mh;
    struct load_command *lc = (struct load_command *)(base + sizeof(struct mach_header_64));
    uint32_t ncmds = ((struct mach_header_64 *)mh)->ncmds;
    int n = 0;
    for (uint32_t i = 0; i < ncmds && n < max; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sg = (struct segment_command_64 *)lc;
            segs[n].vmaddr = sg->vmaddr; segs[n].vmsize = sg->vmsize;
            segs[n].fileoff = sg->fileoff; segs[n].filesize = sg->filesize;
            n++;
        }
        if (lc->cmdsize == 0) break;
        lc = (struct load_command *)((uintptr_t)lc + lc->cmdsize);
    }
    return n;
}

static void *lookup_in_image(const struct mach_header *mh, intptr_t slide, const char *want) {
    if (!mh) return NULL;
    if (mh->magic != MH_MAGIC_64) return NULL;

    seg_t segs[MAX_SEGS];
    int nseg = collect_segments(mh, segs, MAX_SEGS);
    if (nseg == 0) return NULL;

    uintptr_t base = (uintptr_t)mh;
    struct load_command *lc = (struct load_command *)(base + sizeof(struct mach_header_64));
    uint32_t ncmds = ((struct mach_header_64 *)mh)->ncmds;

    for (uint32_t i = 0; i < ncmds; i++) {
        if (lc->cmd == LC_SYMTAB) {
            struct symtab_command *st = (struct symtab_command *)lc;
            if (st->symoff == 0 || st->stroff == 0 || st->nsyms == 0) return NULL;

            /* ★ 用所属 segment 把文件偏移换算成内存地址（不能直接 base+off） */
            uint64_t sym_rel = 0, str_rel = 0;
            if (!file_off_to_addr(segs, nseg, st->symoff,
                                  (uint64_t)st->nsyms * sizeof(struct nlist_64), &sym_rel)) return NULL;
            if (!file_off_to_addr(segs, nseg, st->stroff, st->strsize, &str_rel)) return NULL;

            struct nlist_64 *syms = (struct nlist_64 *)(base + sym_rel);
            char *strtab = (char *)(base + str_rel);

            for (uint32_t s = 0; s < st->nsyms; s++) {
                uint32_t strx = syms[s].n_un.n_strx;
                if (strx == 0 || strx >= st->strsize) continue;   /* 越界保护 */
                char *nm = strtab + strx;
                if (nm[0] != '_') continue;              /* Apple 符号带下划线前缀 */
                if (strcmp(nm + 1, want) == 0 && syms[s].n_value != 0) {
                    return (void *)((uintptr_t)slide + (uintptr_t)syms[s].n_value);
                }
            }
            return NULL;
        }
        if (lc->cmdsize == 0) break;
        lc = (struct load_command *)((uintptr_t)lc + lc->cmdsize);
    }
    return NULL;
}

/* ★ 只扫描 App 包内的镜像。
 * 系统库/dyld 共享缓存里的镜像，其 __LINKEDIT 在共享缓存中是拆分映射的，
 * 用 "镜像基址 + symoff" 去算会落到未映射区域 → 直接闪退。
 * 而 il2cpp 只可能存在于 App 自身（主执行文件或 UnityFramework），
 * 因此系统镜像一律跳过，既安全又更快。 */
static int image_is_app_image(const char *nm)
{
    if (!nm) return 0;
    /* 例： /var/containers/Bundle/Application/xxx/AmongUs.app/AmongUs
     *     .../AmongUs.app/Frameworks/UnityFramework.framework/UnityFramework */
    return strstr(nm, ".app/") != NULL ? 1 : 0;
}

static void *nebula_resolve_symbol(const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    /* 开关：放 nebula_no_scan.txt 可彻底关掉静态符号表扫描（只用 dlsym），
       用于排查闪退是否来自扫描 */
    if (nebula_flag_file("nebula_no_scan.txt")) return NULL;
    /* 兜底：仅遍历 App 包内镜像（主程序 / UnityFramework）查静态符号表 */
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *inm = _dyld_get_image_name(i);
        if (!image_is_app_image(inm)) continue;
        void *r = lookup_in_image(_dyld_get_image_header(i),
                                  _dyld_get_image_vmaddr_slide(i), name);
        if (r) return r;
    }
    return NULL;
}
#else
static void *nebula_resolve_symbol(const char *name) { return dlsym(RTLD_DEFAULT, name); }
#endif

/* ---------- 运行时 ABI 探针：逐个镜像报告 arm64 / arm64e ----------
 * 硬件(M4)和 iOS 内核/系统 App 是 arm64e，但【本 App 自身】可能是 arm64。
 * 决定"替换 methodPointer 会不会被 PAC 验签"的，是本 App 的 ABI：
 *   arm64e  -> 可能有 autia/braa 验签 -> 有 SIGILL 风险
 *   arm64   -> 编译器不生成验签指令   -> 安全
 * 所以这里必须在真机上实测，不能靠猜。 */
void nebula_il2cpp_list_images(void) {
#if defined(__APPLE__)
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct mach_header *mh = _dyld_get_image_header(i);
        const char *nm = _dyld_get_image_name(i);
        if (!mh) continue;
        int cpu = -1, sub = -1;
        if (mh->magic == MH_MAGIC_64) {
            const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
            cpu = (int)h->cputype; sub = (int)(h->cpusubtype & 0xFF);
        } else {
            cpu = (int)mh->cputype; sub = (int)(mh->cpusubtype & 0xFF);
        }
        const char *arch = (cpu == 0x0100000C)
                           ? (sub == 2 ? "arm64e (PAC 风险)" : "arm64 (安全)")
                           : "other";
        nebula_log("image[%u] %-52s cpu=0x%x sub=%d -> %s",
                   i, nm ? nm : "?", cpu, sub, arch);
    }
#endif
}


static void nlog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    nebula_log("%s", buf);   /* 统一走 syslog + 文件 */
}

/* ===================== libil2cpp API 函数指针 ===================== */
typedef void *(*fn_domain_get)(void);
typedef void **(*fn_domain_get_assemblies)(void *domain, size_t *size);
typedef void *(*fn_assembly_get_image)(void *assembly);
typedef size_t (*fn_image_get_class_count)(void *image);
typedef void *(*fn_image_get_class)(void *image, size_t idx);
typedef const char *(*fn_class_get_name)(void *klass);
typedef const char *(*fn_class_get_namespace)(void *klass);
typedef void *(*fn_class_get_fields)(void *klass, void **iter);
typedef void *(*fn_class_get_field_from_name)(void *klass, const char *name);
typedef const char *(*fn_field_get_name)(void *field);
typedef ptrdiff_t (*fn_field_get_offset)(void *field);
typedef void *(*fn_field_get_type)(void *field);
typedef const char *(*fn_type_get_name)(void *type);
typedef void *(*fn_class_get_type)(void *klass);
typedef void *(*fn_type_get_object)(void *type);
typedef void *(*fn_class_get_methods)(void *klass, void **iter);
typedef void *(*fn_class_get_method_from_name)(void *klass, const char *name, int argsCount);
typedef const char *(*fn_method_get_name)(void *method);
typedef int (*fn_method_get_param_count)(void *method);
typedef void *(*fn_runtime_invoke)(void *method, void *obj, void **params, void **exc);
typedef void *(*fn_string_new)(const char *str);

typedef struct {
    fn_domain_get                 domain_get;
    fn_domain_get_assemblies      domain_get_assemblies;
    fn_assembly_get_image         assembly_get_image;
    fn_image_get_class_count      image_get_class_count;
    fn_image_get_class            image_get_class;
    fn_class_get_name             class_get_name;
    fn_class_get_namespace        class_get_namespace;
    fn_class_get_fields           class_get_fields;
    fn_class_get_field_from_name  class_get_field_from_name;
    fn_field_get_name             field_get_name;
    fn_field_get_offset           field_get_offset;
    fn_field_get_type             field_get_type;
    fn_type_get_name              type_get_name;
    fn_class_get_type             class_get_type;
    fn_type_get_object            type_get_object;
    fn_class_get_methods          class_get_methods;
    fn_class_get_method_from_name class_get_method_from_name;
    fn_method_get_name            method_get_name;
    fn_method_get_param_count     method_get_param_count;
    fn_runtime_invoke             runtime_invoke;
    fn_string_new                string_new;
} il2cpp_api_t;

static il2cpp_api_t A = {0};
static int g_api_ok = 0;
static int g_missing = 0;

static void *sym(const char *name) {
    void *p = nebula_resolve_symbol(name);
    if (!p) g_missing++;
    return p;
}

int nebula_il2cpp_init(void) {
    if (g_api_ok) return 0;
    g_missing = 0;

    /* 关键 API：缺任何一个都无法发现类/字段 */
    A.domain_get            = (fn_domain_get)sym("il2cpp_domain_get");
    A.domain_get_assemblies = (fn_domain_get_assemblies)sym("il2cpp_domain_get_assemblies");
    A.assembly_get_image    = (fn_assembly_get_image)sym("il2cpp_assembly_get_image");
    A.image_get_class_count = (fn_image_get_class_count)sym("il2cpp_image_get_class_count");
    A.image_get_class       = (fn_image_get_class)sym("il2cpp_image_get_class");
    A.class_get_name        = (fn_class_get_name)sym("il2cpp_class_get_name");
    A.class_get_field_from_name = (fn_class_get_field_from_name)sym("il2cpp_class_get_field_from_name");
    A.field_get_offset      = (fn_field_get_offset)sym("il2cpp_field_get_offset");
    A.field_get_type        = (fn_field_get_type)sym("il2cpp_field_get_type");
    A.type_get_name         = (fn_type_get_name)sym("il2cpp_type_get_name");

    /* 辅助 API：缺失只影响部分功能（如枚举实例），不致命 */
    A.class_get_namespace   = (fn_class_get_namespace)sym("il2cpp_class_get_namespace");
    A.class_get_fields      = (fn_class_get_fields)sym("il2cpp_class_get_fields");
    A.field_get_name        = (fn_field_get_name)sym("il2cpp_field_get_name");
    A.class_get_type        = (fn_class_get_type)sym("il2cpp_class_get_type");
    A.type_get_object       = (fn_type_get_object)sym("il2cpp_type_get_object");
    A.class_get_methods     = (fn_class_get_methods)sym("il2cpp_class_get_methods");
    A.class_get_method_from_name = (fn_class_get_method_from_name)sym("il2cpp_class_get_method_from_name");
    A.method_get_name       = (fn_method_get_name)sym("il2cpp_method_get_name");
    A.method_get_param_count= (fn_method_get_param_count)sym("il2cpp_method_get_param_count");
    A.runtime_invoke        = (fn_runtime_invoke)sym("il2cpp_runtime_invoke");
    A.string_new            = (fn_string_new)sym("il2cpp_string_new");   /* 辅助：创建字符串 */

    if (!A.domain_get || !A.domain_get_assemblies || !A.assembly_get_image ||
        !A.image_get_class_count || !A.image_get_class || !A.class_get_name ||
        !A.class_get_field_from_name || !A.field_get_offset || !A.field_get_type ||
        !A.type_get_name) {
        nlog("init FAILED: 关键 il2cpp API 缺失（缺 %d 个）。已尝试【动态导出】+"
             "【Mach-O 静态符号表】两条路都找不到。请把 Packages/Among Us.app/Mach-O 的"
             "符号情况告诉我（越狱不可用的话，改用 UnityFramework 内部偏移定位）。", g_missing);
        return -1;
    }
    g_api_ok = 1;
    nlog("init ok: il2cpp API 已解析 %s%d 个符号缺失%s",
         g_missing ? "" : "（", g_missing, g_missing ? "，仅影响枚举实例功能）" : "");
    return 0;
}

int nebula_il2cpp_ready(void) {
    if (!g_api_ok) return 0;
    void *d = A.domain_get();
    if (!d) return 0;
    size_t n = 0;
    void **asms = A.domain_get_assemblies(d, &n);
    return (asms && n > 0) ? 1 : 0;
}

/* ===================== 类发现 ===================== */
void *nebula_il2cpp_find_class_by_hint(const char *substr) {
    if (!g_api_ok || !substr) return NULL;
    void *d = A.domain_get();
    if (!d) return NULL;
    size_t na = 0;
    void **asms = A.domain_get_assemblies(d, &na);
    if (!asms) return NULL;

    for (size_t i = 0; i < na; i++) {
        void *img = A.assembly_get_image(asms[i]);
        if (!img) continue;
        size_t nc = A.image_get_class_count(img);
        for (size_t c = 0; c < nc; c++) {
            void *klass = A.image_get_class(img, c);
            if (!klass) continue;
            const char *nm = A.class_get_name(klass);
            if (nm && strstr(nm, substr)) return klass;
        }
    }
    return NULL;
}

void *nebula_il2cpp_class_get_method(void *klass, const char *name) {
    if (!g_api_ok || !klass || !name) return NULL;
    if (!A.class_get_method_from_name) return NULL;
    return A.class_get_method_from_name(klass, name, -1);  /* -1 = 不限制参数个数 */
}

/* ===================== 字段读写 ===================== */
static void *field_off_and_ptr(void *obj, const char *field, ptrdiff_t *out_off) {
    if (!g_api_ok || !obj || !field) {
        return NULL;
    }
    /* 需要知道 class 才能查字段：从对象头读 klass 指针（offset 0） */
    void *klass = *(void **)obj;
    if (!klass) return NULL;
    void *f = A.class_get_field_from_name(klass, field);
    if (!f) return NULL;
    ptrdiff_t off = A.field_get_offset(f);
    if (out_off) *out_off = off;
    return (char *)obj + off;
}

int nebula_il2cpp_get_float(void *obj, const char *field, float *out) {
    void *p = field_off_and_ptr(obj, field, NULL);
    if (!p || !out) return -1;
    *out = *(float *)p;
    return 0;
}

int nebula_il2cpp_set_float(void *obj, const char *field, float v) {
    void *p = field_off_and_ptr(obj, field, NULL);
    if (!p) return -1;
    *(float *)p = v;
    return 0;
}

int nebula_il2cpp_get_int(void *obj, const char *field, int *out) {
    void *p = field_off_and_ptr(obj, field, NULL);
    if (!p || !out) return -1;
    *out = *(int *)p;
    return 0;
}

int nebula_il2cpp_set_int(void *obj, const char *field, int v) {
    void *p = field_off_and_ptr(obj, field, NULL);
    if (!p) return -1;
    *(int *)p = v;
    return 0;
}

/* ===================== 自适应字段定位 + 职业应用 ===================== */
#include "../core/roles.h"

/* 在类的字段列表里按名字子串匹配（忽略大小写），返回 0 并填充偏移 */
int nebula_il2cpp_find_field(void *klass, const char *substr, ptrdiff_t *off_out)
{
    if (!g_api_ok || !klass || !substr || !A.class_get_fields || !A.field_get_name) return -1;
    size_t sl = strlen(substr);
    void *iter = NULL;
    for (void *f = A.class_get_fields(klass, &iter); f; f = A.class_get_fields(klass, &iter)) {
        const char *fn = A.field_get_name(f);
        if (!fn) continue;
        for (size_t i = 0; fn[i]; i++) {          /* 忽略大小写的子串匹配 */
            size_t j = 0;
            while (j < sl && fn[i + j] &&
                   tolower((unsigned char)fn[i + j]) == tolower((unsigned char)substr[j])) j++;
            if (j == sl) {
                ptrdiff_t off = A.field_get_offset(f);
                if (off_out) *off_out = off;
                return 0;
            }
        }
    }
    return -1;
}

int nebula_il2cpp_get_float_by(void *obj, ptrdiff_t off, float *out)
{
    if (!obj || !out || off < 0) return -1;
    *out = *(float *)((char *)obj + off);
    return 0;
}

int nebula_il2cpp_set_float_by(void *obj, ptrdiff_t off, float v)
{
    if (!obj || off < 0) return -1;
    *(float *)((char *)obj + off) = v;
    return 0;
}

/* Il2CppString：klass(8) + monitor(8) + length(int32,+16) + chars(UTF16,+20) */
int nebula_il2cpp_get_string_by(void *obj, ptrdiff_t off, char *out, size_t osz)
{
    if (!obj || !out || osz == 0 || off < 0) return -1;
    void *s = *(void **)((char *)obj + off);
    if (!s) { out[0] = '\0'; return -1; }
    int32_t len = *(int32_t *)((char *)s + 16);
    const uint16_t *ch = (const uint16_t *)((char *)s + 20);
    if (len < 0 || len > 4096) len = 0;
    size_t w = 0;
    for (int32_t i = 0; i < len && w + 4 < osz; i++) {
        uint32_t c = ch[i];
        if (c < 0x80) out[w++] = (char)c;
        else if (c < 0x800) { out[w++] = (char)(0xC0 | (c >> 6)); out[w++] = (char)(0x80 | (c & 0x3F)); }
        else { out[w++] = (char)(0xE0 | (c >> 12)); out[w++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[w++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[w] = '\0';
    return 0;
}

int nebula_il2cpp_set_string_by(void *obj, ptrdiff_t off, const char *utf8)
{
    if (!obj || !utf8 || off < 0) return -1;
    if (!A.string_new) return -1;                 /* 需要 il2cpp_string_new */
    void *s = A.string_new(utf8);
    if (!s) return -1;
    *(void **)((char *)obj + off) = s;
    return 0;
}

/* ★ 把本地引擎的职业应用到真实游戏对象：
 *   - 每个 PlayerControl 实例按序分配一个官方职业
 *   - 名字前加 [职业中文名]
 *   - 按职业调整移动速度（字段用自适应定位，不写死字段名） */
int nebula_il2cpp_apply_roles(void)
{
    void *klass = nebula_il2cpp_find_class_by_hint("PlayerControl");
    if (!klass) return 0;

    void *insts[16];
    int n = nebula_il2cpp_get_instances(klass, insts, 16);
    if (n <= 0) return 0;

    static const int pool[] = {
        ROLE_SHERIFF, ROLE_JACKAL, ROLE_SNIPER, ROLE_MAYOR,
        ROLE_DOCTOR,  ROLE_SEER,   ROLE_JESTER, ROLE_BAIT
    };
    static const float mul[] = { 1.0f, 1.10f, 1.05f, 0.95f, 1.0f, 1.0f, 1.0f, 1.0f };
    const int pn = (int)(sizeof(pool) / sizeof(pool[0]));

    static ptrdiff_t speed_off = -1, name_off = -1;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        if (nebula_il2cpp_find_field(klass, "speed", &speed_off) == 0)
            nebula_log("apply_roles: 速度字段 +%td", speed_off);
        else nebula_log("apply_roles: 未找到含 speed 的字段");
        if (nebula_il2cpp_find_field(klass, "name", &name_off) == 0)
            nebula_log("apply_roles: 名字字段 +%td", name_off);
        else nebula_log("apply_roles: 未找到含 name 的字段");
    }

    int applied = 0;
    for (int i = 0; i < n; i++) {
        int rid = pool[i % pn];
        const char *rname = nebula_role_name(rid);

        if (speed_off >= 0) {
            float cur = 0;
            if (nebula_il2cpp_get_float_by(insts[i], speed_off, &cur) == 0)
                nebula_il2cpp_set_float_by(insts[i], speed_off, cur * mul[i % pn]);
        }
        if (name_off >= 0) {
            char cur[128];
            if (nebula_il2cpp_get_string_by(insts[i], name_off, cur, sizeof(cur)) == 0) {
                if (!strstr(cur, "[")) {           /* 已加过前缀就不再加 */
                    char nn[160];
                    snprintf(nn, sizeof(nn), "[%s]%s", rname ? rname : "?", cur);
                    nebula_il2cpp_set_string_by(insts[i], name_off, nn);
                }
            }
        }
        applied++;
    }
    return applied;
}

/* ===================== 取类的实例 ===================== */
/* 走 UnityEngine.Object.FindObjectsOfType(System.Type)，返回实例的 native 指针数组 */
int nebula_il2cpp_get_instances(void *klass, void **out, int max) {
    if (!g_api_ok || !klass || !out || max <= 0) return -1;
    if (!A.class_get_type || !A.type_get_object || !A.runtime_invoke ||
        !A.class_get_methods || !A.method_get_name || !A.method_get_param_count) {
        nlog("get_instances: 辅助 API 缺失，无法枚举实例");
        return -1;
    }
    void *objClass = nebula_il2cpp_find_class_by_hint("Object"); /* UnityEngine.Object */
    if (!objClass) { nlog("get_instances: 找不到 UnityEngine.Object"); return -1; }

    /* 找 FindObjectsOfType 的单参数重载 */
    void *iter = NULL, *method = NULL;
    for (void *m = A.class_get_methods(objClass, &iter); m; m = A.class_get_methods(objClass, &iter)) {
        const char *mn = A.method_get_name ? A.method_get_name(m) : NULL;
        if (mn && strcmp(mn, "FindObjectsOfType") == 0 && A.method_get_param_count(m) == 1) {
            method = m; break;
        }
    }
    if (!method) { nlog("get_instances: 找不到 FindObjectsOfType(Type)"); return -1; }

    void *reflType = A.type_get_object(A.class_get_type(klass));
    if (!reflType) { nlog("get_instances: 取不到 System.Type"); return -1; }

    void *params[1] = { reflType };
    void *exc = NULL;
    void *arr = A.runtime_invoke(method, NULL, params, &exc);
    if (!arr) { nlog("get_instances: invoke 返回空%s", exc ? "（有异常）" : ""); return -1; }

    uint32_t len = *(uint32_t *)((char *)arr + ARR_LEN_OFF);
    int n = (int)len; if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = *(void **)((char *)arr + ARR_DATA_OFF + (size_t)i * sizeof(void *));
    return n;
}

/* ===================== 自查 dump（走 syslog，无需越狱） ===================== */
int nebula_il2cpp_dump_report(const char *path, const char **class_hints, int hint_n) {
    if (nebula_il2cpp_init() != 0) return -1;
    if (!nebula_il2cpp_ready()) { nlog("dump: IL2CPP 运行时尚未就绪"); return -1; }

    /* 先报 ABI：arm64 还是 arm64e —— 决定 PAC 风险等级，必须实测 */
    nebula_il2cpp_list_images();

    FILE *fp = path ? fopen(path, "w") : NULL;
    nlog("=== Nebula 自查 dump 开始（类=%d 个线索） ===", hint_n);
    for (int h = 0; h < hint_n; h++) {
        void *klass = nebula_il2cpp_find_class_by_hint(class_hints[h]);
        if (!klass) { nlog("[%s] 未找到该类", class_hints[h]); continue; }
        nlog("[%s] 类已找到", class_hints[h]);
        if (fp) fprintf(fp, "== %s ==\n", class_hints[h]);
        if (!A.class_get_fields || !A.field_get_name || !A.type_get_name) continue;
        void *iter = NULL;
        for (void *f = A.class_get_fields(klass, &iter); f; f = A.class_get_fields(klass, &iter)) {
            const char *fn = A.field_get_name(f);
            const char *tn = A.type_get_name(A.field_get_type(f));
            ptrdiff_t off = A.field_get_offset(f);
            nlog("    field %s : %s @ +%td", fn ? fn : "?", tn ? tn : "?", off);
            if (fp) fprintf(fp, "%-40s %-24s +%td\n", fn ? fn : "?", tn ? tn : "?", off);
        }
        /* 同时输出方法名 —— 要 hook 职业分配/会议流程就得靠它 */
        if (A.class_get_methods && A.method_get_name) {
            void *miter = NULL; int mn = 0;
            for (void *m = A.class_get_methods(klass, &miter); m && mn < 60;
                 m = A.class_get_methods(klass, &miter), mn++) {
                const char *mnm = A.method_get_name(m);
                nlog("    method %s", mnm ? mnm : "?");
                if (fp) fprintf(fp, "  method %s\n", mnm ? mnm : "?");
            }
        }
    }
    if (fp) { fclose(fp); nlog("（同时写入文件 %s）", path); }
    nlog("=== 自查 dump 结束 ===");
    return 0;
}

/* ===================== 第一个可见效果：加速 ===================== */
/* 常见字段名候选；不同 AU 版本叫法不同，逐个尝试 */
static const char *SPEED_FIELDS[] = {
    "moveSpeed", "MoveSpeed", "speed", "Speed", "playerSpeed", "PlayerSpeed",
    "runSpeed", "RunSpeed", NULL
};

int nebula_il2cpp_apply_speed(float multiplier) {
    static int logged_once = 0;
    void *klass = nebula_il2cpp_find_class_by_hint("PlayerControl");
    if (!klass) {
        if (!logged_once) { nlog("apply_speed: 尚未找到 PlayerControl（游戏未进场景？）"); logged_once = 1; }
        return 0;
    }
    void *insts[16];
    int n = nebula_il2cpp_get_instances(klass, insts, 16);
    if (n <= 0) {
        if (!logged_once) { nlog("apply_speed: 取不到 PlayerControl 实例（n=%d）", n); logged_once = 1; }
        return 0;
    }
    int changed = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; SPEED_FIELDS[k]; k++) {
            float cur = 0;
            if (nebula_il2cpp_get_float(insts[i], SPEED_FIELDS[k], &cur) == 0) {
                if (nebula_il2cpp_set_float(insts[i], SPEED_FIELDS[k], cur * multiplier) == 0) changed++;
                if (!logged_once) nlog("apply_speed: 用字段 %s 改了 %d 个实例", SPEED_FIELDS[k], n);
                break;
            }
        }
    }
    logged_once = 1;
    return changed;
}

/* ===================== 后台轮询线程 ===================== */
typedef struct { int interval_ms; } poll_arg_t;
static poll_arg_t g_poll_arg;
static pthread_t g_poll_thr;
static volatile int g_poll_run = 0;

static void *poll_thread_main(void *arg) {
    poll_arg_t *pa = (poll_arg_t *)arg;
    /* ★ 先静置几秒：让 Unity 完成自己的初始化，再开始任何 IL2CPP 相关动作
       （符号解析、ready 探测、dump、hook 全部放到这之后） */
    sleep_ms(5000);
    if (nebula_il2cpp_init() != 0) return NULL;

    for (int i = 0; i < 300 && !nebula_il2cpp_ready(); i++) sleep_ms(200);
    if (!nebula_il2cpp_ready()) { nlog("poll: 等待 IL2CPP 就绪超时"); return NULL; }
    nlog("poll: IL2CPP 已就绪，开始轮询（每 %d ms）", pa->interval_ms);

    /* 就绪后才做自查 dump（类名/字段名/偏移），此时调 IL2CPP 才安全。
       范围要覆盖：玩家/职业/数据/会议/ HUD —— 这些正是接线新职业要用到的类 */
    static const char *hints[] = {
        "PlayerControl", "GameManager", "ShipStatus", "GameData",
        "RoleBehaviour", "PlayerInfo",  "MeetingHud", "AmongUsClient",
        "HudManager",    "PlayerPhysics", "ExileController"
    };
    nebula_il2cpp_dump_report(NULL, hints,
                              (int)(sizeof(hints) / sizeof(hints[0])));

    /* 等级 2：安装指针替换型 hook（无 JIT / 无 Frida），失败则继续纯轮询 */
    int tries = 0, installed = -1;
    while (tries++ < 20 && installed != 0) {
        installed = nebula_hook_install_game_update();
        if (installed != 0) sleep_ms(500);
    }
    nlog("poll: hook 安装结果 %d（0=成功帧级拦截；-1=未找到目标；-2=被开关停用）", installed);
    while (g_poll_run) {
        nebula_il2cpp_apply_speed(1.5f);
        nebula_il2cpp_apply_roles();   /* 职业应用到游戏对象 */
        sleep_ms(pa->interval_ms);
    }
    return NULL;
}

int nebula_il2cpp_start_poll(int interval_ms) {
    if (g_poll_run) return 0;
    if (interval_ms < 50) interval_ms = 50;
    g_poll_arg.interval_ms = interval_ms;
    g_poll_run = 1;
    if (pthread_create(&g_poll_thr, NULL, poll_thread_main, &g_poll_arg) != 0) {
        g_poll_run = 0;
        nlog("poll: 线程创建失败");
        return -1;
    }
    nlog("poll: 后台线程已启动");
    return 0;
}
