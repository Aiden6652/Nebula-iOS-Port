/*
 * boot_overlay.m — 启动占位文本（防 Mono + BepInEx 初始化期间黑屏被误判死机）
 *
 * 背景：宿主 dylib 被注入 Among Us 后，会在后台 pthread 里跑 Mono 虚拟机 +
 * BepInEx chainloader（加载星云舰 Nebula.dll + Harmony 补丁）。这段启动在 iOS
 * 上可能持续数秒；玩家看到“没反应”容易以为黑屏死机。
 *
 * 做法（与 overlay.m 的「职业面板」同一思路，但不依赖它——面板那套走独立 dylib，
 * 本文件直接编进宿主 dylib，保证一定随宿主加载）：
 *   - 在 App 的 UIWindow 上叠一层居中的 UILabel “Nebula 核心正在初始化...”
 *   - 窗口可能还没起来（App 启动早期），故带重试（GCD 定时器，直到装上或超时）
 *   - 托管侧（BepInEx/Nebula）就绪后可经 [DllImport("__Internal")] 调用
 *     nebula_signal_ready() 立刻销毁文字；原生侧也会在 BepInEx 接管后延时销毁 +
 *     最迟 30s 安全兜底销毁，绝不卡死在屏幕上。
 *
 * 不碰 Unity 任何 Canvas / 委托链路（避免注入崩溃风险），纯 UIKit 叠层。
 */

#if defined(__APPLE__)

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <stdlib.h>

/* —— 全局状态（同一 dylib 内，宿主 .c 侧 extern 引用） —— */
static UIView  *g_boot_label = nil;
static volatile int g_boot_want   = 0;   /* 是否应当显示（隐藏后置 0） */
static volatile int g_boot_done   = 0;   /* 是否已销毁 */
static dispatch_source_t g_boot_timer = NULL;

/* 前向声明（extern，非 static，否则会变成文件内局部符号、不导出）：
   下方 boot_arm_safety 在 nebula_hide_boot_overlay 定义前就调用它 */
void nebula_hide_boot_overlay(void);

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

/* 主线程执行：真正创建/刷新居中标签 */
static void boot_install_label(void)
{
    @try {
        UIWindow *win = boot_find_window();
        if (!win) return;   /* 窗口还没起来，等下次重试 */
        if (!g_boot_label) {
            UILabel *lb = [[UILabel alloc] init];
            lb.text = @"Nebula 核心正在初始化...";
            lb.textColor = [UIColor whiteColor];
            lb.backgroundColor = [UIColor clearColor];
            lb.font = [UIFont boldSystemFontOfSize:22];
            lb.textAlignment = NSTextAlignmentCenter;
            lb.numberOfLines = 0;
            lb.lineBreakMode = NSLineBreakByWordWrapping;
            lb.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                                 UIViewAutoresizingFlexibleRightMargin |
                                 UIViewAutoresizingFlexibleTopMargin |
                                 UIViewAutoresizingFlexibleBottomMargin;
            [win addSubview:lb];
            [win bringSubviewToFront:lb];   /* 压在最上层，盖住游戏画面 */
            g_boot_label = lb;
        }
        /* 始终重新居中（窗口尺寸/旋转变化时跟着动） */
        CGFloat W = win.bounds.size.width, H = win.bounds.size.height;
        g_boot_label.bounds = CGRectMake(0, 0, (W > 0 ? W : 320) * 0.92, 64);
        g_boot_label.center = CGPointMake(W / 2.0, H / 2.0);
    } @catch (NSException *e) {
        (void)e;
    }
}

/* 主线程执行：销毁标签 */
static void boot_remove_label(void)
{
    @try {
        if (g_boot_label) { [g_boot_label removeFromSuperview]; g_boot_label = nil; }
    } @catch (NSException *e) {
        (void)e;
    }
}

/* 停止重试定时器 */
static void boot_stop_timer(void)
{
    if (g_boot_timer) {
        dispatch_source_cancel(g_boot_timer);
        g_boot_timer = NULL;
    }
}

/* 启动一个重试定时器：窗口未就绪时每 200ms 重试安装，直到装上或超过 NEBULA_BOOT_OVERLAY_TIMEOUT_MS */
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
        if (g_boot_done || !g_boot_want) { boot_stop_timer(); return; }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_boot_done || !g_boot_want) return;
            boot_install_label();
            if (g_boot_label) { boot_stop_timer(); }   /* 装上了，停定时器 */
        });
    });
    dispatch_resume(t);
}

/* ============ 对外 C 接口（宿主 .c 侧 extern 调用 / 托管侧 DllImport） ============ */

/* 安全兜底：最迟 NEBULA_BOOT_OVERLAY_TIMEOUT_MS（默认 30000ms）后强制销毁，
   防止任何异常路径下文字卡在屏幕上。nebula_signal_ready() 提前销毁后本调用变 no-op。 */
static void boot_arm_safety(void)
{
    int ms = 30000;
    const char *e = getenv("NEBULA_BOOT_OVERLAY_TIMEOUT_MS");
    if (e && *e) { int v = atoi(e); if (v > 0) ms = v; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ nebula_hide_boot_overlay(); });
}

/* 显示启动文本（可多次调用，幂等）。非主线程会转到主线程执行。 */
void nebula_show_boot_overlay(void)
{
    if (g_boot_done) return;
    g_boot_want = 1;
    if ([NSThread isMainThread]) boot_install_label();
    else dispatch_async(dispatch_get_main_queue(), ^{ boot_install_label(); });
    boot_arm_retry();
    boot_arm_safety();
}

/* 销毁启动文本（可多次调用，幂等）。非主线程会转到主线程执行。 */
void nebula_hide_boot_overlay(void)
{
    if (g_boot_done) return;
    g_boot_done = 1;
    g_boot_want = 0;
    boot_stop_timer();
    if ([NSThread isMainThread]) boot_remove_label();
    else dispatch_async(dispatch_get_main_queue(), ^{ boot_remove_label(); });
}

/* 托管侧（BepInEx / Nebula）就绪信号：DllImport("__Internal") 可直达。
   语义：Nebula.dll 已加载且 Harmony 补丁就绪 → 立刻销毁占位文字，
   把画面交还给游戏（让星云舰自己的加载动画露出来）。 */
void nebula_signal_ready(void)
{
    nebula_hide_boot_overlay();
}

#endif /* __APPLE__ */
