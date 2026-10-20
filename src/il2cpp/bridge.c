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
#include "../core/freeplay.h"
#include "../core/roleweight.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>      /* GCD：把操作排到游戏主线程，无需 hook */
#endif
#include <ctype.h>

/* 日志文件上限：超过即截断重写。
   日志每轮都写，长期运行会撑满设备存储（进而引发 OOM / 卡死），必须限。 */
#ifndef NEBULA_LOG_MAX_BYTES
#define NEBULA_LOG_MAX_BYTES  (2 * 1024 * 1024)   /* 2 MB */
#endif
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
        /* ★ 存储防护：日志不能无限增长（每轮都写，久了会撑满磁盘）。
           超过上限就截断重写，只保留最新内容。 */
        FILE *chk = fopen(path, "rb");
        if (chk) {
            fseek(chk, 0, SEEK_END);
            long sz = ftell(chk);
            fclose(chk);
            if (sz > NEBULA_LOG_MAX_BYTES) {
                FILE *tr = fopen(path, "w");       /* 超限：清空重来 */
                if (tr) { fprintf(tr, "[Nebula-iOS] （日志超过 %d KB，已截断，以下为最新）\n",
                                  NEBULA_LOG_MAX_BYTES / 1024); fclose(tr); }
            }
        }
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
typedef int      (*fn_string_length)(void *str);
typedef uint16_t *(*fn_string_chars)(void *str);
typedef void *(*fn_string_new)(const char *str);
typedef void  (*fn_field_static_get)(void *field, void *value);

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
    fn_string_length              string_length;
    fn_string_chars               string_chars;
    fn_string_new                string_new;
    fn_field_static_get          field_static_get;
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
    A.string_length         = (fn_string_length)sym("il2cpp_string_length");
    A.string_chars          = (fn_string_chars)sym("il2cpp_string_chars");
    A.string_new            = (fn_string_new)sym("il2cpp_string_new");   /* 辅助：创建字符串 */
    A.field_static_get      = (fn_field_static_get)sym("il2cpp_field_static_get_value"); /* 读静态字段 */

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
#include "../core/rolecfg.h"
#include "../core/rolemapping.h"

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

/* ★ 精确定位字段：按【完整字段名】依次尝试候选，全部失败才放弃。
   之前用子串匹配（如 "name"）会命中非目标字段并写入 → 破坏邻近数据。
   改为精确名后，宁可找不到不写，也绝不错写。 */
int nebula_il2cpp_find_field_exact(void *klass, const char **names, ptrdiff_t *off_out)
{
    if (!g_api_ok || !klass || !names || !A.class_get_field_from_name ||
        !A.field_get_offset) return -1;
    for (int i = 0; names[i]; i++) {
        void *f = A.class_get_field_from_name(klass, names[i]);
        if (!f) continue;
        ptrdiff_t off = A.field_get_offset(f);
        if (off < 0) continue;
        if (off_out) *off_out = off;
        return 0;
    }
    return -1;
}

int nebula_il2cpp_get_float_by(void *obj, ptrdiff_t off, float *out)
{
    if (!obj || !out || off < 0) return -1;
    *out = *(float *)((char *)obj + off);
    return 0;
}

int nebula_il2cpp_set_int_by(void *obj, ptrdiff_t off, int v)
{
    if (!obj || off < 0) return -1;
    *(int *)((char *)obj + off) = v;
    return 0;
}

int nebula_il2cpp_get_int_by(void *obj, ptrdiff_t off, int *out)
{
    if (!obj || !out || off < 0) return -1;
    *out = *(int *)((char *)obj + off);
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


/* ★ 设置玩家名字：新版 AU 里 PlayerName 不是字段，而是属性
   （NetworkedPlayerInfo.get_PlayerName / set_PlayerName），
   真实数据存在 Outfits 字典里的 PlayerOutfit 中。
   所以不能靠字段偏移写，必须【调用方法】。 */
int nebula_il2cpp_set_player_name(void *info_obj, const char *utf8)
{
    if (!g_api_ok || !info_obj || !utf8) return -1;
    void *klass = *(void **)info_obj;
    if (!klass) return -1;
    if (!A.class_get_method_from_name || !A.runtime_invoke || !A.string_new) return -1;

    void *m = A.class_get_method_from_name(klass, "set_PlayerName", 1);
    if (!m) m = A.class_get_method_from_name(klass, "SetPlayerName", 1);
    if (!m) return -1;

    void *s = A.string_new(utf8);
    if (!s) return -1;
    void *params[1] = { s };
    void *exc = NULL;
    A.runtime_invoke(m, info_obj, params, &exc);
    return exc ? -1 : 0;
}

/* ★ 调用【无参】方法（返回 void 或忽略返回值）。
   用途：调原版自带的 GameData.AddDummy 补齐房间人数 ——
   星云舰自由模式就是靠这类 dummy 把房间填满，让一个人也能开局。 */
int nebula_il2cpp_call_void(void *obj, const char *method)
{
    if (!g_api_ok || !obj || !method) return -1;
    void *klass = *(void **)obj;
    if (!klass) return -1;
    if (!A.class_get_method_from_name || !A.runtime_invoke) return -1;

    void *m = A.class_get_method_from_name(klass, method, -1);
    if (!m) m = A.class_get_method_from_name(klass, method, 0);
    if (!m) return -1;

    void *params[1] = { NULL };
    void *exc = NULL;
    A.runtime_invoke(m, obj, params, &exc);
    return exc ? -1 : 0;
}

/* ★ 读玩家真名：新版 AU 里名字是【属性】（get_PlayerName），字段读不到时用它。
   必须能读到真名，否则写名字会把玩家的昵称冲掉。 */
int nebula_il2cpp_get_player_name(void *info_obj, char *out, size_t osz)
{
    if (!g_api_ok || !info_obj || !out || osz == 0) return -1;
    void *klass = *(void **)info_obj;
    if (!klass) return -1;
    if (!A.class_get_method_from_name || !A.runtime_invoke) return -1;
    if (!A.string_length || !A.string_chars) return -1;

    void *m = A.class_get_method_from_name(klass, "get_PlayerName", -1);
    if (!m) m = A.class_get_method_from_name(klass, "GetPlayerName", -1);
    if (!m) return -1;

    void *params[1] = { NULL };
    void *exc = NULL;
    void *ret = A.runtime_invoke(m, info_obj, params, &exc);
    if (exc || !ret) return -1;

    int len = A.string_length(ret);
    const uint16_t *ch = A.string_chars(ret);
    if (len <= 0 || !ch) return -1;
    if ((size_t)len > osz - 1) len = (int)osz - 1;

    /* UTF-16 → UTF-8（BMP 足够：玩家名不会用 emoji 平面外的字符） */
    size_t o = 0;
    for (int i = 0; i < len && o + 4 < osz; i++) {
        uint32_t c = ch[i];
        if (c == 0) break;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o > 0 ? 0 : -1;
}

/* ★ 自由模式：把【开局最小人数】从 4 改成 1。
   原版 GameStartManager 要求房间里至少 4 个人才能按开始 ——
   这是"开了自由模式也启动不了"的直接原因（一个人永远凑不够 4）。
   字段按名动态查找，不需要预先知道偏移。 */
static void freeplay_lower_min_players(void)
{
    if (!nebula_freeplay_enabled()) return;

    static int done = 0, miss = 0;
    if (done) return;

    void *klass = nebula_il2cpp_find_class_by_hint("GameStartManager");
    if (!klass) {
        if (++miss % 20 == 1)
            nebula_log("freeplay: 未找到 GameStartManager 类（第%d次）", miss);
        return;
    }
    void *inst[4];
    int n = nebula_il2cpp_get_instances(klass, inst, 4);
    if (n <= 0) return;                      /* 不在大厅就没有实例，正常 */

    ptrdiff_t off = -1;
    if (nebula_il2cpp_find_field(klass, "MinPlayers", &off) != 0) {
        if (++miss % 20 == 1)
            nebula_log("freeplay: GameStartManager 里没有 MinPlayers 字段（第%d次）", miss);
        return;
    }
    int cur = -1;
    if (nebula_il2cpp_get_int_by(inst[0], off, &cur) == 0 && cur != 1) {
        if (nebula_il2cpp_set_int_by(inst[0], off, 1) == 0) {
            done = 1;
            nebula_log("freeplay: 开局最小人数 %d → 1（一个人也能开始）", cur);
        }
    } else if (cur == 1) {
        done = 1;
    }
}

/* ★ 自由模式：把房间人数自动补齐到目标人数。
   调原版 GameData.AddDummy（游戏自带方法，不是我们造的）。
   每轮只补【一个】：一次补一堆容易触发游戏内部同步问题/卡顿。 */
/* iOS 悬浮按钮（src/ui/overlay.m 提供）：自由模式下点它切换职业。
   weak 符号 —— 没链接 overlay.m 时为 NULL，调用处必须判空。 */
int nebula_ui_switch_requested(void) __attribute__((weak));

/* 装面板（可反复调用，装好就返回 1）。dylib 加载时常拿不到窗口，
   必须持续重试，否则用户永远看不到按钮。 */
int nebula_ui_install_now(void) __attribute__((weak));

/* ★ 配置热重载：用户在 iPad「文件」App 里改了 nebula_roles.txt /
   nebula_freeplay.txt，1 秒内生效，不用重启游戏。
   这也是面板万一装不上时的兜底手段（改 role xxx 就能指定职业）。 */
#include <sys/stat.h>
static time_t nebula_file_mtime(const char *path)
{
    struct stat st;
    if (!path || !path[0]) return 0;
    return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

static void reload_if_changed(const char *path, time_t *last_mtime,
                              int (*reload)(const char *), const char *tag)
{
    if (!path || !path[0] || !last_mtime || !reload) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtime == *last_mtime) return;
    int first = (*last_mtime == 0);
    *last_mtime = st.st_mtime;
    reload(path);
    if (!first) nebula_log("%s: 检测到配置改动，已热重载", tag);
}

/* 每轮都试着装一次面板，装好后就不再试（开销极小） */
static void freeplay_try_install_ui(void)
{
    static int done = 0, tries = 0;
    if (done || !nebula_ui_install_now) return;
    if (nebula_ui_install_now()) { done = 1; return; }
    if (++tries % 20 == 1)
        nebula_log("面板：第 %d 次尝试创建仍未拿到窗口（会继续重试）", tries);
}

/* 是否在大厅：LobbyBehaviour 只在大厅场景存在。
   大厅里不写职业名 —— 没开局就亮出 [警长] 等于剧透自己（真机截图已证实）。 */
static int in_lobby(void)
{
    void *lk = nebula_il2cpp_find_class_by_hint("LobbyBehaviour");
    if (!lk) return 0;
    void *li[2];
    return nebula_il2cpp_get_instances(lk, li, 2) > 0;
}

/* ★ 调用无参方法并取 int 返回值（用于 GameData.get_PlayerCount）。
   IL2CPP 里 int 返回是【装箱对象】：对象头(klass 8 + monitor 8)之后是值。 */
int nebula_il2cpp_call_int(void *obj, const char *method, int *out)
{
    if (!g_api_ok || !obj || !method || !out) return -1;
    void *klass = *(void **)obj;
    if (!klass) return -1;
    if (!A.class_get_method_from_name || !A.runtime_invoke) return -1;

    void *m = A.class_get_method_from_name(klass, method, -1);
    if (!m) return -1;

    void *params[1] = { NULL };
    void *exc = NULL;
    void *ret = A.runtime_invoke(m, obj, params, &exc);
    if (exc || !ret) return -1;

    *out = *(int32_t *)((char *)ret + 16);   /* 装箱 int 的值 */
    return 0;
}

/* ★ 自由模式：把房间人数自动补齐到目标人数。
   调原版 GameData.AddDummy（游戏自带方法，不是我们造的）。

   ⚠ 关键：判断"现在几个人"必须读【房间真实人数】(GameData.get_PlayerCount)，
   不能用局内 PlayerControl 实例数 —— dummy 未必有 PlayerControl 实例，
   用后者会永远觉得人不够，每轮补一个把房间撑爆。
   另外无论读不读得到人数，总补次数都有硬上限，杜绝无限补。 */
static int freeplay_fill_room(int fallback_players)
{
    int target = nebula_freeplay_fill();
    if (target <= 0) return 0;

    static int failed = 0, added = 0;
    if (failed) return 0;            /* 试过一次失败就不再试（防每轮抛异常） */
    if (added >= target) return 0;   /* ★ 硬保险：最多补 target 个 */

    void *gdk = nebula_il2cpp_find_class_by_hint("GameData");
    if (!gdk) return 0;
    void *inst[4];
    int m = nebula_il2cpp_get_instances(gdk, inst, 4);
    if (m <= 0) {
        static int miss = 0;
        if (++miss % 10 == 1)
            nebula_log("freeplay: 取不到 GameData 实例(第%d次) —— 暂无法补齐人数", miss);
        return 0;
    }

    int cur = -1;
    if (nebula_il2cpp_call_int(inst[0], "get_PlayerCount", &cur) == 0) {
        if (cur >= target) return 0;                 /* 人数已够 */
    } else if (fallback_players > 0 && fallback_players >= target) {
        return 0;                                    /* 读不到人数时用局内实例数兜底 */
    }

    if (nebula_il2cpp_call_void(inst[0], "AddDummy") != 0) {
        failed = 1;
        nebula_log("freeplay: AddDummy 调用失败 —— 停止补齐（不影响正常游戏）");
        return 0;
    }
    added++;
    if (added <= 3 || added % 5 == 0)
        nebula_log("freeplay: 已补 %d 个 dummy（目标 %d 人，当前 %d）",
                   added, target, cur);
    return 1;
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

/* ★ 把职业名写进【身份/角色显示界面】
 *
 * 之前改玩家名字不显示，因为身份显示在专门的 UI（开局介绍页 IntroCutscene、
 * 任务/身份面板 TaskPanelBehaviour 等），那些文字是 TextMeshProUGUI 的 m_text。
 *
 * 这里用【内容识别】定位，不必知道具体是哪个 UI 元素：
 *   遍历所有 TextMeshProUGUI → 读 m_text →
 *   若内容等于某个基础职业名（船员/伪装者/Scientist…）→ 换成我们的职业名。
 *   开局介绍页、任务面板上出现的职业文字都会被命中。
 */
int nebula_il2cpp_apply_role_text(void)
{
    void *klass = nebula_il2cpp_find_class_by_hint("TextMeshProUGUI");
    if (!klass) { klass = nebula_il2cpp_find_class_by_hint("TextMeshPro"); }
    if (!klass) return 0;

    static ptrdiff_t text_off = -1;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        static const char *tf[] = {"m_text", "text", "m_Text", NULL};
        nebula_il2cpp_find_field_exact(klass, tf, &text_off);
        if (text_off >= 0) nebula_log("role_text: 文本字段 +%td", text_off);
        else               nebula_log("role_text: 未找到文本字段（将跳过）");
    }
    if (text_off < 0) return 0;

    /* 默认【开启】：把原版职业槽位的文字换成我们的职业名。
       这样【编辑房间（局外）】和身份界面都能看到我们的职业 —— 用户要求
       "必须在局内能看到、知道开了什么"。
       已做防护：主线程执行(GCD) + 每轮最多新建 8 个字符串 + 实例数上限 128。
       若要关闭，放 nebula_no_text.txt。 */
    /* ★ 原版 11 个槽位【废弃】：它装不下 267 个职业，也加不进条目。
       职业设置全交由我们自己的面板接管，所以默认不再往原版 UI 里写职业名。
       真想恢复旧行为，放一个 nebula_role_slots.txt 即可。 */
    if (!nebula_flag_file("nebula_role_slots.txt")) return 0;
    if (nebula_flag_file("nebula_no_text.txt")) return 0;

    void *insts[128];
    int n = nebula_il2cpp_get_instances(klass, insts, 128);
    if (n <= 0) {
        /* 诊断：枚举不到实例是"没有职业显示"的最常见原因，必须能看到 */
        static int miss = 0;
        if (++miss % 10 == 1)
            nebula_log("role_text: 取不到 UI 文本实例(第%d次，n=%d) —— 职业名替换无法进行",
                       miss, n);
        return 0;
    }
    /* 诊断：把实际看到的文本打出来，便于核对槽位名是否匹配（中文/英文/带符号） */
    {
        static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            char sample[128]; int shown = 0;
            for (int i = 0; i < n && shown < 10; i++) {
                if (nebula_il2cpp_get_string_by(insts[i], text_off, sample, sizeof(sample)) != 0) continue;
                if (!sample[0]) continue;
                nebula_log("role_text: 文本样本[%d] = %s", shown, sample);
                shown++;
            }
        }
    }

    /* 上场职业（与 apply_roles 共用配置） */
    static int roster[ROLECFG_MAX]; static int roster_n = 0; static int cfg_done = 0;
    if (!cfg_done) {
        cfg_done = 1;
        const char *home = getenv("HOME");
        char cfg[1024] = {0};
        if (home) snprintf(cfg, sizeof(cfg), "%s/Documents/nebula_roles.txt", home);
        nebula_rolecfg_emit_default(home ? cfg : NULL);
        nebula_rolecfg_load(home ? cfg : NULL, roster, ROLECFG_MAX, &roster_n);
        /* 导出全部职业清单，便于在 iPad「文件」App 里查看有什么职业可挑 */
        if (home) {
            char allp[1024];
            snprintf(allp, sizeof(allp), "%s/Documents/nebula_roles_all.txt", home);
            if (nebula_rolecfg_export_all(allp) == 0)
                nebula_log("已导出全部职业清单: Documents/nebula_roles_all.txt（%d 个）",
                           nebula_role_count());
        }
    }
    if (roster_n <= 0) nebula_rolecfg_default(roster, ROLECFG_MAX, &roster_n);

    /* 需要被替换掉的【基础职业显示名】（中英都列，兼容不同语言） */
    /* ★ 槽位映射：原版角色设置界面有 11 个固定槽位，
       把每个槽位的文字换成我们阵容里【对应顺序】的职业名。
       这样你在【编辑房间】里看到的就是我们的职业，且能直接调数量
       （原版列表是硬编码加不进条目，只能借它的槽位显示）。 */
    static const char *slots[][3] = {
        {"科学家",   "Scientist",     "Scientist"},
        {"工程师",   "Engineer",      "Engineer"},
        {"守护天使", "Guardian Angel","GuardianAngel"},
        {"变形者",   "Shapeshifter",  "Shapeshifter"},
        {"幻影",     "Phantom",       "Phantom"},
        {"追踪者",   "Tracker",       "Tracker"},
        {"噪音制造者","Noisemaker",   "Noisemaker"},
        {"侦探",     "Detective",     "Detective"},
        {"审判官",   "Judge",         "Judge"},
        {"通灵师",   "SpiritGuide",   "SpiritGuide"},
        {"毒蛇",     "Viper",         "Viper"},
        {NULL, NULL, NULL}
    };
    static const char *team_names[] = {"船员", "伪装者", "内鬼", "Crewmate", "Impostor", NULL};

    /* ★ 内存防护：每轮最多新建这么多个字符串。
       il2cpp_string_new 每次都从托管堆分配，若游戏把名字重置回去，
       我们会反复分配；限流后即使发生也不会堆积成 OOM。 */
    const int ALLOC_BUDGET = 8;
    int replaced = 0;
    for (int i = 0; i < n && replaced < ALLOC_BUDGET; i++) {
        char cur[128];
        if (nebula_il2cpp_get_string_by(insts[i], text_off, cur, sizeof(cur)) != 0) continue;
        if (!cur[0]) continue;
        if (strstr(cur, "[")) continue;                 /* 已替换过 */

        /* ① 阵营名不动（船员/伪装者） */
        int is_team = 0;
        for (int t = 0; team_names[t]; t++)
            if (strcmp(cur, team_names[t]) == 0) { is_team = 1; break; }
        if (is_team) continue;

        /* ② 命中哪个槽位 → 换成对应顺序的职业名 */
        for (int k = 0; slots[k][0]; k++) {
            if (strcmp(cur, slots[k][0]) == 0 ||
                strcmp(cur, slots[k][1]) == 0 ||
                strcmp(cur, slots[k][2]) == 0) {

                int rid = (k < roster_n) ? roster[k] : -1;
                const char *rname = rid >= 0 ? nebula_role_name(rid) : NULL;
                char nn[160];
                if (rname)
                    snprintf(nn, sizeof(nn), "[%s]", rname);
                else
                    snprintf(nn, sizeof(nn), "[空位%d]", k + 1);   /* 该槽位未分配 */

                if (nebula_il2cpp_set_string_by(insts[i], text_off, nn) == 0) {
                    nebula_log("role_text: 槽位%d %s -> %s", k + 1, cur, nn);
                    replaced++;
                }
                break;
            }
        }
    }
    if (replaced) nebula_log("role_text: 本轮替换 %d 处（槽位→职业）", replaced);
    return replaced;
}

/* ---- 读取静态字段（如 PlayerControl.AllPlayerControls） ---- */
static int static_field_ptr(void *klass, const char *name, void **out)
{
    if (!g_api_ok || !klass || !name || !A.class_get_field_from_name || !A.field_static_get) return -1;
    void *f = A.class_get_field_from_name(klass, name);
    if (!f) return -1;
    void *v = NULL;
    A.field_static_get(f, &v);
    if (!v) return -1;
    if (out) *out = v;
    return 0;
}

/* ---- 读取 List<T>：返回元素指针数组 ---- */
static int list_elements(void *list_obj, void **out, int max)
{
    if (!list_obj || !out || max <= 0) return -1;
    void *klass = *(void **)list_obj;
    if (!klass) return -1;
    ptrdiff_t items_off = -1, size_off = -1;
    if (nebula_il2cpp_find_field(klass, "_items", &items_off) != 0) return -1;
    nebula_il2cpp_find_field(klass, "_size", &size_off);
    void *arr = *(void **)((char *)list_obj + items_off);
    if (!arr) return -1;
    int size = (size_off >= 0) ? *(int *)((char *)list_obj + size_off) : 0;
    if (size <= 0) size = *(int32_t *)((char *)arr + 0x18);
    if (size <= 0) return -1;
    if (size > max) size = max;
    for (int i = 0; i < size; i++)
        out[i] = *(void **)((char *)arr + 0x20 + (size_t)i * sizeof(void *));
    return size;
}

/* ★ 取所有玩家：优先静态 PlayerControl.AllPlayerControls，退化为 FindObjectsOfType */
int nebula_il2cpp_get_players(void **out, int max)
{
    void *klass = nebula_il2cpp_find_class_by_hint("PlayerControl");
    if (!klass) return -1;
    void *list = NULL;
    if (static_field_ptr(klass, "AllPlayerControls", &list) == 0 && list) {
        int n = list_elements(list, out, max);
        if (n > 0) return n;
    }
    return nebula_il2cpp_get_instances(klass, out, max);
}

static float g_base_speed = 0.0f;   /* 基准速度，只记录一次 */

int nebula_il2cpp_apply_roles(void)
{
    /* ★ 必须在最开头：大厅里没有玩家实例，后面的代码会直接 return，
       放在那儿的话面板按钮永远装不上。 */
    freeplay_try_install_ui();

    void *klass = nebula_il2cpp_find_class_by_hint("PlayerControl");
    if (!klass) return 0;

    void *insts[16];
    int n = nebula_il2cpp_get_players(insts, 16);
    if (n <= 0) {
        /* 取不到玩家 = 当前不在对局内（大厅/编辑房间没有 PlayerControl 实例）。
           这是正常的：职业显示在【局内玩家名字上方】，局外本来就没有。 */
        static int miss_log = 0;
        if (++miss_log % 10 == 1)
            nebula_log("apply_roles: 当前无玩家实例(第%d次) —— 若一直为 0，说明尚未进入对局",
                       miss_log);
        return 0;
    }

    /* ★ 上场职业来自【权重表】（267 个职业，由游戏内「职业」面板调整）。
       每轮重新展开：面板里点了 +/- 之后 1 秒内就生效，不用重启游戏。 */
    static int roster[512]; static int roster_n = 0; static int cfg_done = 0;
    static char g_roles_path[1024] = {0}, g_fp_path[1024] = {0};
    static time_t g_roles_mtime = 0, g_fp_mtime = 0;
    if (!cfg_done) {
        cfg_done = 1;
        const char *home = getenv("HOME");
        if (home) snprintf(g_roles_path, sizeof(g_roles_path),
                           "%s/Documents/nebula_roles.txt", home);
        nebula_rolecfg_emit_default(home ? g_roles_path : NULL);   /* 首次生成默认清单 */
        nebula_weight_load(home ? g_roles_path : NULL);
        g_roles_mtime = nebula_file_mtime(g_roles_path);
        /* ★ 自由模式（星云舰 Solo Free Play）：存在 nebula_freeplay.txt 即开启 */
        if (home) {
            char fp[1024] = {0};
            snprintf(fp, sizeof(fp), "%s/Documents/nebula_freeplay.txt", home);
            if (nebula_freeplay_load(fp) != 0)
                nebula_freeplay_emit_sample(fp);   /* 首次生成示例（默认关闭） */
            snprintf(g_fp_path, sizeof(g_fp_path), "%s", fp);
            g_fp_mtime = nebula_file_mtime(fp);
        }
    }
    reload_if_changed(g_roles_path, &g_roles_mtime, nebula_weight_load, "roleweight");
    reload_if_changed(g_fp_path,    &g_fp_mtime,    nebula_freeplay_load, "freeplay");
    roster_n = nebula_weight_expand(roster, 512);
    /* 自由模式：先把房间人数补齐（单人也能开局） */
    freeplay_fill_room(n);
    /* 再把原版"至少 4 人才开始"的限制降到 1 人 */
    freeplay_lower_min_players();
    if (roster_n <= 0) nebula_rolecfg_default(roster, ROLECFG_MAX, &roster_n);

    /* 速度：PlayerControl.MyPhysics -> PlayerPhysics.speed
       速度效果默认关闭：历史 bug 就是它每轮 read→×1.5→write，
       10 秒后速度达 3300 倍，玩家飞出地图、交互全部失效。
       需要时放 nebula_speed.txt 开启，且用【基准值 × 系数】写入。 */
    static ptrdiff_t phys_off = -1, speed_off = -1;
    static int speed_tried = 0;
    if (!nebula_flag_file("nebula_speed.txt")) {
        if (!speed_tried) {
            speed_tried = 1;
            nebula_log("apply_roles: 速度效果默认关闭（放 nebula_speed.txt 开启）");
        }
    } else if (speed_off < 0) {
        /* 只要还没定位成功就重试（游戏刚进场景时对象可能尚未创建）。
           注意：不能用 speed_tried 一次性锁死，否则"先关后开"会永久失效。 */
        if (nebula_il2cpp_find_field(klass, "MyPhysics", &phys_off) == 0 && n > 0) {
            void *p0 = *(void **)((char *)insts[0] + phys_off);
            if (p0) {
                void *pk = *(void **)p0;
                static const char *sp[] = {"Speed", "speed", "MoveSpeed", "moveSpeed", NULL};
                if (nebula_il2cpp_find_field_exact(pk, sp, &speed_off) == 0)
                    nebula_log("apply_roles: 速度 PlayerPhysics +%td", speed_off);
            }
        }
    }

    /* 底层角色类型：NetworkedPlayerInfo.RoleType（原版 RoleTypes 枚举） */
    static ptrdiff_t rt_off = -1;
    static int rt_tried = 0, rt_logged = 0, name_logged = 0;
    /* 自由模式下允许反复重试：大厅阶段玩家数为 0 定位不到字段，
       不重试的话进局后映射就永远失效（带刀职业拿不到击杀按钮）。 */
    if (!rt_tried || (nebula_freeplay_enabled() && rt_off < 0)) {
        rt_tried = 1;
        ptrdiff_t io = -1;                       /* 块内局部定位，避免依赖后面的变量 */
        if (nebula_il2cpp_find_field(klass, "CachedPlayerData", &io) == 0 && n > 0) {
            void *i0 = *(void **)((char *)insts[0] + io);
            if (i0) {
                void *ik = *(void **)i0;
                static const char *rt[] = {"RoleType", "roleType", NULL};
                if (nebula_il2cpp_find_field_exact(ik, rt, &rt_off) == 0)
                    nebula_log("apply_roles: 底层角色字段 RoleType +%td", rt_off);
            }
        }
        if (rt_off < 0) nebula_log("apply_roles: 未找到 RoleType 字段（底层映射跳过）");
    }

    /* 名字：PlayerControl.CachedPlayerData -> NetworkedPlayerInfo.PlayerName */
    static ptrdiff_t info_off = -1, name_off = -1;
    static int name_tried = 0;
    if (!name_tried) {
        name_tried = 1;
        if (nebula_il2cpp_find_field(klass, "CachedPlayerData", &info_off) == 0 && n > 0) {
            void *i0 = *(void **)((char *)insts[0] + info_off);
            if (i0) {
                void *ik = *(void **)i0;
                static const char *nm[] = {"PlayerName", "playerName", "playername", NULL};
                nebula_il2cpp_find_field_exact(ik, nm, &name_off);
                if (name_off >= 0)
                    nebula_log("apply_roles: 名字 NetworkedPlayerInfo +%td", name_off);
            }
        }
        if (name_off < 0) nebula_log("apply_roles: 名字字段未定位（将跳过）");
    }

    /* 自由模式：点悬浮按钮 → 立即切换到下一个职业（星云舰 FreeplayPopover 的对应做法） */
    if (nebula_freeplay_enabled() && nebula_ui_switch_requested
        && nebula_ui_switch_requested()) {
        nebula_freeplay_next(roster_n);
        int nr = nebula_freeplay_role(0, roster, roster_n);
        nebula_log("freeplay: 按钮 → 切换职业 -> %s",
                   nr >= 0 && nebula_role_name(nr) ? nebula_role_name(nr) : "?");
    }
    /* 自由模式：到点就换清单里的下一个职业（一个人逐个试用） */
    if (nebula_freeplay_tick(roster_n, 0)) {
        int nr = nebula_freeplay_role(0, roster, roster_n);
        const char *rn = nr >= 0 ? nebula_role_name(nr) : NULL;
        nebula_log("freeplay: 切换试用职业 -> %s", rn ? rn : "?");
    }

    int applied = 0;
    for (int i = 0; i < n; i++) {
        /* 自由模式下：0 号（自己）拿固定/轮换的职业，其余按清单排 */
        int rid = nebula_freeplay_role(i, roster, roster_n);
        if (rid < 0) continue;
        const char *rname = nebula_role_name(rid);
        nebula_team_t team = nebula_role_team(rid);
        nebula_core_register_player((uint32_t)(i + 1), team);

        /* 速度：用【基准值 × 系数】写入，绝不读当前值再乘（那会指数爆炸） */
        if (phys_off >= 0 && speed_off >= 0) {
            void *phys = *(void **)((char *)insts[i] + phys_off);
            if (phys) {
                float base = 0;
                if (nebula_il2cpp_get_float_by(phys, speed_off, &base) == 0 && base > 0.0f) {
                    /* 基准速度只记录一次（第一次看到的合理值） */
                    if (g_base_speed <= 0.0f && base < 32.0f) g_base_speed = base;
                    if (g_base_speed > 0.0f) {
                        float m = (team == TEAM_IMPOSTOR) ? 1.08f
                                : (team == TEAM_NEUTRAL)  ? 1.04f : 1.0f;
                        nebula_il2cpp_set_float_by(phys, speed_off, g_base_speed * m);
                    }
                }
            }
        }
        /* ★ 底层枚举映射：把我们的职业伪装成原版 RoleTypes。
           带刀（内鬼/豺狼/警长）→ Impostor，功能型 → Crewmate。
           不这样做的话，豺狼和警长在游戏底层是 Crewmate，拿不到击杀按钮。 */
        if (info_off >= 0 && rt_off >= 0) {
            void *info = *(void **)((char *)insts[i] + info_off);
            if (info) {
                int underlying = nebula_role_underlying(
                        rid, nebula_role_key(rid), team == TEAM_IMPOSTOR);
                int cur_rt = -1;
                if (nebula_il2cpp_get_int_by(info, rt_off, &cur_rt) != 0 || cur_rt != underlying) {
                    if (nebula_il2cpp_set_int_by(info, rt_off, underlying) == 0) {
                        if (!rt_logged) {
                            rt_logged = 1;
                            nebula_log("apply_roles: 底层角色映射已生效（带刀→Impostor=%d，功能型→Crewmate=%d）",
                                       RT_IMPOSTOR, RT_CREWMATE);
                        }
                    }
                }
            }
        }
        void *info = (info_off >= 0) ? *(void **)((char *)insts[i] + info_off) : NULL;

        /* 名字：新版 AU 的 PlayerName 是【属性(方法)】不是字段
           （NetworkedPlayerInfo.set_PlayerName），真实数据在 Outfits 字典里，
           所以必须用方法调用写入，不能靠字段偏移。 */
        /* 自由模式下强制显示职业名（用户要靠它确认试的是哪个职业）；
           但【大厅除外】—— 没开局就亮职业是剧透（真机已证实此 bug）。 */
        int show_name = !in_lobby() &&
                        (nebula_freeplay_enabled() || !nebula_flag_file("nebula_no_name.txt"));
        if (info && show_name) {
            char cur[192]; cur[0] = 0;
            if (name_off >= 0) nebula_il2cpp_get_string_by(info, name_off, cur, sizeof(cur));
            /* 字段读不到（新版 AU 名字是属性）→ 走 get_PlayerName 方法，
               必须拿到真名，否则会把玩家的昵称冲掉。 */
            if (!cur[0]) nebula_il2cpp_get_player_name(info, cur, sizeof(cur));
            /* 剥掉我们自己加过的前缀：取最后一个换行之后 = 玩家本名 */
            const char *base = strrchr(cur, '\n');
            base = base ? base + 1 : cur;

            /* 只在【期望名字 ≠ 当前名字】时才写：
               既避免反复分配字符串（防 OOM），又保证自由模式轮换职业后名字会更新。 */
            {
                /* 自由模式轮换时把进度写进名字：一眼看出试到第几个 */
                char prog[32] = {0}, label[64];
                if (i == 0 && nebula_freeplay_progress(prog, sizeof(prog)))
                    snprintf(label, sizeof(label), "%s %s", prog, rname ? rname : "?");
                else
                    snprintf(label, sizeof(label), "%s", rname ? rname : "?");

                char want[192];
                if (nebula_flag_file("nebula_plain_name.txt")) {
                    snprintf(want, sizeof(want), "[%s]\n%s", label, base);
                } else {
                    const char *col = nebula_role_color(rid);
                    snprintf(want, sizeof(want),
                             "<size=70%%><color=%s>[%s]</color></size>\n%s",
                             col ? col : "#FFFFFF", label, base);
                }
                if (strcmp(cur, want) != 0) {
                    if (nebula_il2cpp_set_player_name(info, want) == 0 && !name_logged) {
                        name_logged = 1;
                        nebula_log("apply_roles: 名字已写入（走 set_PlayerName 方法）");
                    }
                }
            }
        }
        applied++;
    }
    {
        static int round = 0;
        if (++round % 10 == 1)
            nebula_log("apply_roles: 玩家=%d 已应用=%d | 名字字段%s 速度字段%s",
                       n, applied,
                       name_off  >= 0 ? "已定位" : "未定位",
                       speed_off >= 0 ? "已定位" : "未定位(默认关)");
    }
    return applied;
}

/* ===================== 取类的实例 ===================== */
/* 走 UnityEngine.Object.FindObjectsOfType(System.Type)，返回实例的 native 指针数组 */
/* 精确定位 UnityEngine.Object：子串搜索会先命中 GameObject 之类，
   导致 FindObjectsOfType 找不到 → 一个实例都取不到 */
static void *unity_object_class(void)
{
    if (!g_api_ok || !A.class_get_namespace || !A.class_get_name) return NULL;
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
            const char *ns = A.class_get_namespace(klass);
            const char *nm = A.class_get_name(klass);
            if (ns && nm && strcmp(ns, "UnityEngine") == 0 && strcmp(nm, "Object") == 0)
                return klass;
        }
    }
    return NULL;
}

int nebula_il2cpp_get_instances(void *klass, void **out, int max) {
    if (!g_api_ok || !klass || !out || max <= 0) return -1;
    /* ★ method_get_param_count 只用于辅助过滤，不能因为它缺失就放弃枚举。
       之前把它列为必需项，一旦解析不到就整个枚举失败 ——
       导致 apply_role_text 取不到任何 UI 文本，职业名替换完全不生效。 */
    if (!A.class_get_type || !A.type_get_object || !A.runtime_invoke) {
        nlog("get_instances: 关键 API 缺失，无法枚举实例");
        return -1;
    }
    void *objClass = unity_object_class();
    if (!objClass) { nlog("get_instances: 找不到 UnityEngine.Object"); return -1; }

    /* 直接用 IL2CPP 接口按名取方法（比遍历方法表可靠） */
    void *method = NULL;
    if (A.class_get_method_from_name) {
        method = A.class_get_method_from_name(objClass, "FindObjectsOfType", 1);
        if (!method) method = A.class_get_method_from_name(objClass, "FindObjectsByType", 1);
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

    /* 后台线程到此为止：只负责定时把任务投递到主线程。
       ★ 这里【一个 IL2CPP 调用都不能有】—— 连 ready() 都不查，
       因为它内部会调 il2cpp_domain_get()。就绪判断改在主线程的
       main_init_once() 里做（未就绪就下次再试，安全且无跨线程调用）。 */
    nlog("poll: 已启动（%d ms 一次），IL2CPP 操作全部经 GCD 投递到游戏主线程", pa->interval_ms);
    while (g_poll_run) {
        nebula_il2cpp_apply_all_async();
        sleep_ms(3000);
    }
    return NULL;
}

/* ★ 只在【主线程】做一次的重活：自查 dump + 安装 hook。
   以前放在后台 pthread 里 —— dump 要扫 11 个类的全部字段与方法
   （上千次 IL2CPP 调用），跨线程会破坏 Unity 内部状态，
   正是"默认配置下游戏依然异常"的真凶。现在挪到主线程。 */
static int g_main_inited = 0;
static void main_init_once(void)
{
    if (g_main_inited) return;
    if (!nebula_il2cpp_ready()) return;      /* 未就绪就下次再试，不硬来 */
    g_main_inited = 1;

    static const char *hints[] = {
        "PlayerControl", "GameManager", "ShipStatus", "GameData",
        "RoleBehaviour", "PlayerInfo",  "MeetingHud", "AmongUsClient",
        "HudManager",    "PlayerPhysics", "ExileController",
        /* ★ 官方内置模组 API（iOS 版也有！）—— 摸清它的结构就能：
           显示模组标记(合规)、注册模组、甚至让官方服务器认这是模组房 */
        "ModManager", "ModStamp",
        "GameStartManager", "LobbyBehaviour"   /* 大厅/开局相关（自由模式要用） */
    };
    nebula_il2cpp_dump_report(NULL, hints, (int)(sizeof(hints)/sizeof(hints[0])));

    int rc = nebula_hook_install_game_update();
    nlog("主线程初始化: hook 安装结果 %d（0=成功；-2=默认停用；-3=未取到原入口已撤销）", rc);
}

/* 一次性把职业应用到游戏（名字显示 + 可选的身份界面文本替换） */
void nebula_il2cpp_apply_all(void)
{
    main_init_once();
    nebula_il2cpp_apply_roles();
    nebula_il2cpp_apply_role_text();
}

/* ★ 把 IL2CPP 操作排到【游戏主线程】执行。
   Unity/IL2CPP 的 API 不是线程安全的，绝不能在后台 pthread 里直接调用
   （会破坏 Unity 内部状态，表现为交互失效、按键无响应）。
   用 GCD 主队列即可排到主线程，且【不依赖 hook】—— 官方 API，
   比 hook 侵入式接管 FixedUpdate 安全得多。 */
void nebula_il2cpp_apply_all_async(void)
{
#if defined(__APPLE__) && defined(__BLOCKS__)
    dispatch_async(dispatch_get_main_queue(), ^{
        nebula_il2cpp_apply_all();
    });
#else
    nebula_il2cpp_apply_all();      /* 非 Apple（本地测试无 GCD）：直接执行 */
#endif
}

/* ★ 游戏主线程每帧调用（由 hook 的 tick 回调驱动）。
   这里执行 IL2CPP 操作是安全的（在主线程内、且在游戏自己的 Update 里）。
   内部限流：默认每 60 帧（约 1 秒）真正执行一次，避免每帧扫描拖慢游戏。 */
void nebula_il2cpp_tick_from_game(void)
{
    static int frame = 0;
    if (++frame < 60) return;
    frame = 0;
    if (!nebula_il2cpp_ready()) return;
    nebula_il2cpp_apply_roles();        /* 职业登记 + 名字前缀（+可选速度） */
    nebula_il2cpp_apply_role_text();    /* 职业名写进身份显示界面 */
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
