/*
 * boot_overlay.m — [R60] 启动品牌页「AUI Mod Loader by Aiden」，[R73] 强制播放 5 秒
 *
 * ⛔⛔⛔ 版权 / 署名红线（原创作者 Aiden）—— 永久保留，禁止删除本页 ⛔⛔⛔
 *   本文件实现的品牌页「AUI Mod Loader by Aiden」是作者 Aiden 的原创署名与
 *   作者凭证，用于防止他人接手后把这份 iOS 移植成果「摸权篡位」说成自己写的。
 *   【任何后续版本 / 任何接手者，绝对不能删、不能改名、不能替换、不能隐藏本页】：
 *     - 不得删除本文件、不得删除 boot_install_label() 里的 UILabel / 黑底 UIView；
 *     - 不得改动 @"AUI Mod Loader by Aiden" 这行署名文字（含文案、作者名、样式）；
 *     - 每次启动 nebula_show_boot_overlay() 必须照常显示本页。
 *   【只允许改「逻辑」】：显示时长、销毁时机、放行闸门（由 NOS 就绪信号驱动）、
 *     透明层级与动画衔接、保活/兜底定时器参数。页面实体与署名文字永远保留。
 *   本红线见 /workspace/交接文档_Nebula-iOS-Port.md 开头「硬红线」章节。
 *   任何删除/改名都会体现在 git diff 里，一眼可查 —— 请勿触碰。
 * ⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔⛔
 *
 * 背景：R58/R59 的「正在生成interop...」占位在 R60 已彻底废除 —— 设备端永不生成
 * interop（131 个 iOS 桥接 dll 由主 dylib 内嵌、启动自动释放到 BepInEx/interop/）。
 * [R73] 用户要求：每次启动强制播放 "AUI Mod Loader by Aiden" 满 5 秒（原 R60 是 3s），
 *   给「主线程 JIT 预热」留一段安全窗口 —— 阶段A(Start) 成功后由主线程把装插件
 *   链路的托管方法提前 mono_compile_method 编好，避免阶段B 新线程首次 JIT 崩。
 *
 * 做法（纯 UIKit，不碰 Unity 任何 Canvas / 委托链路，避免注入崩溃）：
 *   - 在 App 的 UIWindow 上叠一层黑底 + 居中白字（窗口早期未就绪则 GCD 重试安装）
 *   - 首次显示起算 5 秒内，任何 hide / signal_ready 请求都只登记、不真正销毁，
 *     由主队列保活定时器在满 5s 时移除 —— 即「强制播放」，Nebula 就绪也不得提前销毁
 *   - 保活定时器每 100ms 打在主队列上，主线程持续有事件，规避 iOS 看门狗误杀
 *   - 最迟 30s 安全兜底强行移除，绝不卡屏
 */

#if defined(__APPLE__)

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

/* —— 全局状态（同一 dylib 内，宿主 .c 侧 extern 引用） —— */
static UIView  *g_boot_label = nil;
static UIView  *g_boot_bg    = nil;
static volatile int g_boot_want         = 0;
static volatile int g_boot_done         = 0;
static volatile int g_boot_hide_pending = 0;
static volatile int g_boot_ready  = 0;   /* [R98] NOS 就绪：宿主 PatchAll 完成 */
static volatile int g_boot_min_ok = 0;   /* [R98] 首次显示起最短展示时长已满 */
static dispatch_source_t g_boot_timer     = NULL;
static dispatch_source_t g_boot_keepalive = NULL;
static double g_boot_shown_at = 0.0;         /* gettimeofday 秒，首次显示时刻 */
static int    g_boot_min_ms   = 5000;        /* [R73] 强制最短展示时长 5s（原 R60 = 3000） */

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

/* 真正销毁（内部，绕过 5s 门槛；安全兜底与保活定时器满 5s 时调用） */
static void boot_do_hide(void)
{
    if (g_boot_done) return;
    g_boot_done = 1;
    g_boot_want = 0;
    boot_stop_timer();
    boot_stop_keepalive();
    /* [R98] 淡出衔接：品牌页 0.35s 淡出再移除，让底层 Innersloth 动画 / NOS
       加载动画无缝露出来。改的只是销毁时机与动画 —— 页面实体、黑底 UIView、
       @"AUI Mod Loader by Aiden" 署名文字在显示期间始终存在（硬红线）。 */
    void (^fade)(void) = ^{
        [UIView animateWithDuration:0.35 animations:^{
            g_boot_bg.alpha = 0.0;
            g_boot_label.alpha = 0.0;
        } completion:^(BOOL done){ (void)done; boot_remove_label(); }];
    };
    if ([NSThread isMainThread]) fade();
    else dispatch_async(dispatch_get_main_queue(), fade);
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
            lb.text = @"AUI Mod Loader by Aiden";   // ⛔ 原创署名（Aiden）：永久保留，禁止删/改名/替换/隐藏 —— 见文件头红线
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

/* 保活定时器：① 主线程持续有事件（规避看门狗）；② 满 5s 自动销毁品牌页 */
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
        if ((boot_now() - g_boot_shown_at) * 1000.0 >= (double)g_boot_min_ms)
            g_boot_min_ok = 1;
        /* [R98] 双闸门：最短展示时长满足 + NOS 就绪（PatchAll 完成）才淡出。
           NOS 还在 patch 时品牌页持续盖住 —— 绝不放行到原版主菜单画面。 */
        if (g_boot_min_ok && g_boot_ready) boot_do_hide();
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
    /* [R98] PatchAll 最快也要几十秒，30s 会把品牌页提前删掉；拉到 120s，
       且这只是视觉兜底，不放行权仍归 NOS 原生闸门（AllowFinishLoadingScene）。 */
    int ms = 120000;
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

/* 请求销毁（可多次调用，幂等）。[R73] 只登记，满 5s 才真正移除。 */
void nebula_hide_boot_overlay(void)
{
    if (g_boot_done) return;
    g_boot_hide_pending = 1;
    if ((boot_now() - g_boot_shown_at) * 1000.0 >= (double)g_boot_min_ms)
        g_boot_min_ok = 1;
    /* [R98] 只登记「最短时长已满足」，销毁必须再等 NOS 就绪（双闸门）。 */
    if (g_boot_min_ok && g_boot_ready) boot_do_hide();
}

/* 托管侧（BepInEx / Nebula）就绪信号：DllImport("__Internal") 可直达。
   [R98] 宿主在 NebulaManualExecute / PatchAll 完成后调这里 —— 页面一行不删，
   只把销毁条件改成「最短展示时长 + NOS 就绪」双闸门（硬红线只允许改逻辑）。 */
void nebula_signal_ready(void)
{
    g_boot_ready = 1;
    printf("[r98][overlay] NOS 就绪信号已到（最短时长 %s）\n",
           g_boot_min_ok ? "已满足，立即淡出" : "未满，等保活定时器");
    if (g_boot_min_ok) boot_do_hide();
}

#endif /* __APPLE__ */
