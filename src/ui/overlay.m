/*
 * overlay.m — iOS 悬浮按钮：自由模式下点它切换职业
 *
 * 背景：星云舰的自由模式有一个职业选择弹窗（FreeplayPopover）。
 * 我们不注入 Unity UI（创建 Canvas/Button/委托链路太长、易崩），
 * 而是走更稳的 iOS 原生层：在 App 的 UIWindow 上加一个 UIKit 按钮。
 * 点击只设置一个标志位，真正的切职业在【游戏主线程】的 apply_roles 里做
 * （IL2CPP 操作必须在 Unity 主线程，绝不跨线程碰游戏对象）。
 *
 * 编译：仅 iOS/macOS 构建（dylib Makefile）。其它平台整段为空。
 */

#if defined(__APPLE__)

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

static volatile int g_nebula_switch = 0;

/* bridge.c（weak 符号）每轮查询：返回 1 表示用户点过按钮 */
int nebula_ui_switch_requested(void)
{
    int v = g_nebula_switch;
    g_nebula_switch = 0;
    return v;
}

@interface NebulaFPHandler : NSObject
- (void)onSwitch:(id)sender;
@end

@implementation NebulaFPHandler
- (void)onSwitch:(id)sender
{
    (void)sender;
    g_nebula_switch = 1;
}
@end

__attribute__((constructor))
static void nebula_ui_install(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *win = nil;
        UIApplication *app = [UIApplication sharedApplication];
        /* iOS 13+ 多场景；拿前台活跃的 UIWindowScene 的第一个窗口 */
        for (UIScene *sc in app.connectedScenes) {
            if ([sc isKindOfClass:[UIWindowScene class]] &&
                sc.activationState == UISceneActivationStateForegroundActive) {
                win = ((UIWindowScene *)sc).windows.firstObject;
                if (win) break;
            }
        }
        if (!win) win = app.keyWindow;   /* 兜底（旧 App 生命周期） */
        if (!win) return;

        UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
        btn.frame = CGRectMake(win.bounds.size.width - 100.0, 96.0, 88.0, 36.0);
        [btn setTitle:@"换职业" forState:UIControlStateNormal];
        btn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
        btn.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.4];
        btn.layer.cornerRadius = 10;
        btn.layer.borderWidth = 1;
        btn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.6].CGColor;
        btn.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                               UIViewAutoresizingFlexibleBottomMargin;
        /* 确保能收到触摸：不挡住下面 Unity 视图的其它区域 */
        btn.userInteractionEnabled = YES;

        NebulaFPHandler *h = [NebulaFPHandler new];
        objc_setAssociatedObject(btn, &g_nebula_switch, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [btn addTarget:h action:@selector(onSwitch:) forControlEvents:UIControlEventTouchUpInside];
        [win addSubview:btn];
    });
}
#endif /* __APPLE__ */
