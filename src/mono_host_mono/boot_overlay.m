/*
 * boot_overlay.m — [R60] 启动品牌页「AUI Mod Loader by Aiden」，强制播放 3 秒
 *
 * 背景：R58/R59 的「正在生成interop...」占位在 R60 已彻底废除 —— 设备端永不生成
 * interop（131 个 iOS 桥接 dll 由主 dylib 内嵌、启动自动释放到 BepInEx/interop/）。
 * 用户明确要求：每次启动强制播放 "AUI Mod Loader by Aiden" 满 3 秒。
 *
 * 做法（纯 UIKit，不碰 Unity 任何 Canvas / 委托链路，避免注入崩溃）：
 *   - 在 App 的 UIWindow 上叠一层黑底 + 居中白字（窗口早期未就绪则 GCD 重试安装）
 *   - 首次显示起算 3 秒内，任何 hide / signal_ready 请求都只登记、不真正销毁，
 *     由主队列保活定时器在满 3s 时移除 —— 即「强制播放」，Nebula 就绪也不得提前销毁
 *   - 保活定时器每 100ms 打在主队列上，主线程持续有事件，规避 iOS 看门狗误杀
 *   - 最迟 30s 安全兜底强行移除，绝不卡屏
 */

#if defined(__APPLE__)

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

/* —— 全局状态（同一 dylib 内，宿主 .c 侧 extern 引用） —— */
static UIView  *g_boot_label = nil;
static UIView  *g_boot_bg    = nil;
static volatile int g_boot_want         = 0;
static volatile int g_boot_done         = 0;
static volatile int g_boot_hide_pending = 0;
static dispatch_source_t g_boot_timer     = NULL;
static dispatch_source_t g_boot_keepalive = NULL;
static double g_boot_shown_at = 0.0;         /* gettimeofday 秒，首次显示时刻 */
static int    g_boot_min_ms   = 3000;        /* 强制最短展示时长 */

static double boot_now(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static UIWindow *boot_find_window(void)
{
    UIApplication *app = [UIApplication sharedApplication];
    if (!app) return nil;
    for (UIScene *sc in app.connectedScenes) {
        if ([sc isKindOfClass:[UIWindowScene class]] &&
            sc.activationState == UISceneActivationStateForegroundActive) {
            for (UIWindow *w in ((UIWindowScene *)sc).windows) if (w) return w;
        }
    }
    for (UIScene *sc in app.connectedScenes) {
        if ([sc isKindOfClass:[UIWindowScene class]])
            for (UIWindow *w in ((UIWindowScene *)sc).windows) if (w) return w;
    }
    if ([app respondsToSelector:@selector(keyWindow)]) return app.keyWindow;
    return nil;
}

static void boot_stop_timer(void)
{
    if (g_boot_timer) {
        dispatch_source_cancel(g_boot_timer);
        g_boot_timer = NULL;
    }
}

static void boot_stop_keepalive(void)
{
    if (g_boot_keepalive) {
        dispatch_source_cancel(g_boot_keepalive);
        g_boot_keepalive = NULL;
    }
}

static void boot_remove_label(void)
{
    @try {
        if (g_boot_label) { [g_boot_label removeFromSuperview]; g_boot_label = nil; }
        if (g_boot_bg)    { [g_boot_bg removeFromSuperview];    g_boot_bg = nil; }
    } @catch (NSException *e) {
        (void)e;
    }
}

/* 真正销毁（内部，绕过 3s 门槛；安全兜底与保活定时器满 3s 时调用） */
static void boot_do_hide(void)
{
    if (g_boot_done) return;
    g_boot_done = 1;
    g_boot_want = 0;
    boot_stop_timer();
    boot_stop_keepalive();
    if ([NSThread isMainThread]) boot_remove_label();
    else dispatch_async(dispatch_get_main_queue(), ^{ boot_remove_label(); });
}

/* 主线程执行：真正创建/刷新品牌页 */
static void boot_install_label(void)
{
    @try {
        UIWindow *win = boot_find_window();
        if (!win) return;   /* 窗口还没起来，等下次重试 */
        if (!g_boot_bg) {
            UIView *bg = [[UIView alloc] initWithFrame:win.bounds];
            bg.backgroundColor = [UIColor blackColor];
            bg.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                  UIViewAutoresizingFlexibleHeight;
            [win addSubview:bg];
            g_boot_bg = bg;
        }
        g_boot_bg.frame = win.bounds;
        if (!g_boot_label) {
            UILabel *lb = [[UILabel alloc] init];
            lb.text = @"AUI Mod Loader by Aiden";
            lb.textColor = [UIColor whiteColor];
            lb.backgroundColor = [UIColor clearColor];
            lb.font = [UIFont boldSystemFontOfSize:26];
            lb.textAlignment = NSTextAlignmentCenter;
            lb.numberOfLines = 0;
            lb.lineBreakMode = NSLineBreakByWordWrapping;
            lb.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                                  UIViewAutoresizingFlexibleRightMargin |
                                  UIViewAutoresizingFlexibleTopMargin |
                                  UIViewAutoresizingFlexibleBottomMargin;
            [win addSubview:lb];
            [win bringSubviewToFront:lb];
            g_boot_label = lb;
        }
        CGFloat W = win.bounds.size.width, H = win.bounds.size.height;
        g_boot_label.bounds = CGRectMake(0, 0, (W > 0 ? W : 320) * 0.92, 120);
        g_boot_label.center = CGPointMake(W / 2.0, H / 2.0);
    } @catch (NSException *e) {
        (void)e;
    }
}

/* 保活定时器：① 主线程持续有事件（规避看门狗）；② 满 3s 自动销毁品牌页 */
static void boot_start_keepalive(void)
{
    if (g_boot_keepalive || g_boot_done) return;
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                 dispatch_get_main_queue());
    if (!t) return;
    g_boot_keepalive = t;
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0),
                              100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{
        if (g_boot_done) { boot_do_hide(); return; }
        if ((boot_now() - g_boot_shown_at) * 1000.0 >= (double)g_boot_min_ms) {
            boot_do_hide();   /* 强制满 3s 才移除，与 g_boot_hide_pending 无关 */
        }
    });
    dispatch_resume(t);
}

/* 窗口未就绪时每 200ms 重试安装，直到装上或已销毁 */
static void boot_arm_retry(void)
{
    if (g_boot_timer || g_boot_done) return;
    dispatch_queue_t q = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0);
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    if (!t) return;
    g_boot_timer = t;
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0),
                              200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{
        if (g_boot_done) { boot_stop_timer(); return; }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_boot_done) return;
            boot_install_label();
            if (g_boot_label) boot_stop_timer();   /* 装上了，停重试 */
        });
    });
    dispatch_resume(t);
}

/* ============ 对外 C 接口（宿主 .c 侧 extern 调用 / 托管侧 DllImport） ============ */

/* 安全兜底：最迟 30s 后强制销毁，防止任何异常路径下品牌页卡屏。
   NEBULA_BOOT_OVERLAY_TIMEOUT_MS 可覆盖。 */
static void boot_arm_safety(void)
{
    int ms = 30000;
    const char *e = getenv("NEBULA_BOOT_OVERLAY_TIMEOUT_MS");
    if (e && *e) { int v = atoi(e); if (v > 0) ms = v; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ boot_do_hide(); });
}

/* 显示品牌页（可多次调用，幂等）。非主线程会转到主线程执行。 */
void nebula_show_boot_overlay(void)
{
    if (g_boot_done) return;
    if (!g_boot_want) {
        g_boot_want = 1;
        g_boot_shown_at = boot_now();
        const char *m = getenv("NEBULA_BOOT_OVERLAY_MIN_MS");
        if (m && *m) { int v = atoi(m); if (v > 0) g_boot_min_ms = v; }
    }
    if ([NSThread isMainThread]) boot_install_label();
    else dispatch_async(dispatch_get_main_queue(), ^{ boot_install_label(); });
    boot_arm_retry();
    boot_start_keepalive();
    boot_arm_safety();
}

/* 请求销毁（可多次调用，幂等）。[R60] 只登记，满 3s 才真正移除。 */
void nebula_hide_boot_overlay(void)
{
    if (g_boot_done) return;
    g_boot_hide_pending = 1;
    if ((boot_now() - g_boot_shown_at) * 1000.0 >= (double)g_boot_min_ms)
        boot_do_hide();
}

/* 托管侧（BepInEx / Nebula）就绪信号：DllImport("__Internal") 可直达。
   [R60] 语义改为「登记就绪」，不再提前销毁品牌页（强制播满 3s）。 */
void nebula_signal_ready(void)
{
    nebula_hide_boot_overlay();
}

#endif /* __APPLE__ */
