// case_insensitive_fs.c —— iOS 文件系统大小写敏感，而 PC(Windows) 不敏感。
// 所以星云包在 PC 上能跑，并不代表路径大小写是对的：搬上 iOS 后 BepInEx / Mono
// 按精确路径加载 dll 会 FileNotFound，加载器直接崩。
//
// 这里用 fishhook 重绑 libc 的文件访问函数：精确路径打不开(ENOENT)时，
// 逐级做大小写不敏感匹配，找到磁盘上的真实文件名再打开。
// 只在"精确失败"时才回退，正常路径无额外开销。
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fishhook.h"

static int   (*orig_open)(const char *, int, ...);
static int   (*orig_openat)(int, const char *, int, ...);
static FILE *(*orig_fopen)(const char *, const char *);
static int   (*orig_stat)(const char *, struct stat *);
static int   (*orig_access)(const char *, int);
static DIR  *(*orig_opendir)(const char *);

// 逐级大小写不敏感解析：返回磁盘上的真实路径(malloc，调用方 free)，失败返回 NULL。
// 注意内部一律用 orig_*，避免再次走进被 hook 的函数造成递归。
static char *resolve_ci(const char *path)
{
    if (!path || !*path) return NULL;

    char abs[PATH_MAX];
    if (path[0] != '/') {
        if (!getcwd(abs, sizeof(abs))) return NULL;
        size_t n = strlen(abs);
        snprintf(abs + n, sizeof(abs) - n, "/%s", path);
    } else {
        snprintf(abs, sizeof(abs), "%s", path);
    }

    struct stat st;
    if (orig_stat && orig_stat(abs, &st) == 0) return NULL;   // 精确存在就别折腾

    char out[PATH_MAX];
    out[0] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(abs, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "/%s", out);
        DIR *d = orig_opendir ? orig_opendir(parent) : opendir(parent);
        if (!d) return NULL;
        struct dirent *e;
        char found[256] = {0};
        int hit = 0;
        while ((e = readdir(d))) {
            if (strcasecmp(e->d_name, tok) == 0) {
                snprintf(found, sizeof(found), "%s", e->d_name);
                hit = 1;
                break;
            }
        }
        closedir(d);
        if (!hit) return NULL;
        size_t n = strlen(out);
        snprintf(out + n, sizeof(out) - n, "%s%s", n ? "/" : "", found);
    }

    size_t len = strlen(out) + 2;
    char *res = (char *)malloc(len);
    if (!res) return NULL;
    snprintf(res, len, "/%s", out);
    return res;
}

int my_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int r = orig_open ? orig_open(path, flags, mode) : -1;
    if (r < 0 && errno == ENOENT) {
        char *fx = resolve_ci(path);
        if (fx) { r = orig_open(fx, flags, mode); free(fx); }
    }
    return r;
}

int my_openat(int fd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int r = orig_openat ? orig_openat(fd, path, flags, mode) : -1;
    if (r < 0 && errno == ENOENT) {
        char *fx = resolve_ci(path);
        if (fx) { r = orig_openat(fd, fx, flags, mode); free(fx); }
    }
    return r;
}

FILE *my_fopen(const char *path, const char *mode)
{
    FILE *f = orig_fopen ? orig_fopen(path, mode) : NULL;
    if (!f) {
        char *fx = resolve_ci(path);
        if (fx) { f = orig_fopen(fx, mode); free(fx); }
    }
    return f;
}

int my_stat(const char *path, struct stat *buf)
{
    int r = orig_stat ? orig_stat(path, buf) : -1;
    if (r != 0) {
        char *fx = resolve_ci(path);
        if (fx) { r = orig_stat(fx, buf); free(fx); }
    }
    return r;
}

int my_access(const char *path, int mode)
{
    int r = orig_access ? orig_access(path, mode) : -1;
    if (r != 0) {
        char *fx = resolve_ci(path);
        if (fx) { r = orig_access(fx, mode); free(fx); }
    }
    return r;
}

DIR *my_opendir(const char *path)
{
    DIR *d = orig_opendir ? orig_opendir(path) : NULL;
    if (!d) {
        char *fx = resolve_ci(path);
        if (fx) { d = orig_opendir(fx); free(fx); }
    }
    return d;
}

void nebula_cifs_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    if (getenv("NEBULA_CIFS_OFF")) {
        fprintf(stderr, "[cifs] 已按 NEBULA_CIFS_OFF 关闭\n");
        return;
    }
    struct rebinding rb[] = {
        {"open",    (void *)my_open,    (void **)&orig_open},
        {"openat",  (void *)my_openat,  (void **)&orig_openat},
        {"fopen",   (void *)my_fopen,   (void **)&orig_fopen},
        {"stat",    (void *)my_stat,    (void **)&orig_stat},
        {"access",  (void *)my_access,  (void **)&orig_access},
        {"opendir", (void *)my_opendir, (void **)&orig_opendir},
    };
    rebind_symbols(rb, sizeof(rb) / sizeof(rb[0]));
    fprintf(stderr, "[cifs] 大小写不敏感回退已安装\n");
}
