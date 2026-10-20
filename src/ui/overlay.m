/*
 * overlay.m — 原生层动态生成的【职业面板】（接管 267 个职业的设置）
 *
 * 背景：原版大厅的身份调整列表只有 11 个固定槽位，装不下 267 个职业，
 * 也加不进条目。所以原版那 11 个槽位【废弃】，改由本面板接管。
 *
 * 做法（不注入 Unity UI，避免 Canvas/委托链路的崩溃风险）：
 *   - 在 App 的 UIWindow 上叠一层 UIKit 视图
 *   - 悬浮按钮「职业」打开/收起面板
 *   - 面板 = 工具条(自由模式开关 + 换职业 + 收起) + 可滚动的 267 行列表
 *   - 每行：职业名 + 权重 + [-]/[+] 按钮，点了直接改 C 层权重并落盘
 *
 * 数据读写全部走 src/ui/roles_ui.h 的 C 接口（可本地测试）。
 * 编译：仅 iOS 构建（dylib Makefile）。
 */

#if defined(__APPLE__)

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include "roles_ui.h"
#include "../il2cpp/bridge.h"   /* nebula_log */

/* 由 bridge 侧反复调用直到装好（weak 链接，见 bridge.c） */
int nebula_ui_install_now(void);

/* —— 悬浮按钮点击标志（bridge.c 每轮查询） —— */
static volatile int g_nebula_switch = 0;

int nebula_ui_switch_requested(void)
{
    int v = g_nebula_switch;
    g_nebula_switch = 0;
    return v;
}

static UIView         *g_panel = nil;
static UIScrollView   *g_list  = nil;
static NSMutableArray *g_weight_labels = nil;   /* 每行的权重 Label，点 +/- 后刷新 */

@interface NebulaPanelCtl : NSObject
- (void)onToggle:(id)sender;     /* 悬浮按钮：打开/收起面板 */
- (void)onNextRole:(id)sender;   /* 换职业 */
- (void)onFreeplay:(id)sender;   /* 自由模式开关 */
- (void)onMinus:(id)sender;      /* 权重 -1 */
- (void)onPlus:(id)sender;       /* 权重 +1 */
- (void)refreshRow:(int)idx;
@end

@implementation NebulaPanelCtl

- (void)refreshRow:(int)idx
{
    if (idx < 0 || idx >= (int)g_weight_labels.count) return;
    UILabel *lb = g_weight_labels[idx];
    int w = nebula_ui_role_weight(idx);
    lb.text = [NSString stringWithFormat:@"%d", w];
    lb.textColor = (w > 0) ? [UIColor systemGreenColor] : [UIColor lightGrayColor];
}

- (void)onToggle:(id)sender
{
    (void)sender;
    if (!g_panel) return;
    BOOL show = g_panel.hidden;
    if (show) {
        /* 每次打开按当前权重刷新（可能刚被其它逻辑改过） */
        for (NSInteger i = 0; i < g_weight_labels.count; i++) [self refreshRow:(int)i];
    }
    g_panel.hidden = !show;
}

- (void)onNextRole:(id)sender
{
    (void)sender;
    g_nebula_switch = 1;
}

- (void)onFreeplay:(id)sender
{
    UISwitch *sw = (UISwitch *)sender;
    nebula_ui_set_freeplay(sw.on ? 1 : 0);
}

- (void)onMinus:(id)sender
{
    int idx = (int)((UIButton *)sender).tag;
    nebula_ui_role_bump(idx, -1);
    [self refreshRow:idx];
}

- (void)onPlus:(id)sender
{
    int idx = (int)((UIButton *)sender).tag;
    nebula_ui_role_bump(idx, +1);
    [self refreshRow:idx];
}

@end

/* 前置声明：安装函数在 build_panel / docs_dir 定义之前使用它们 */
static NSString *docs_dir(void);
static void build_panel(UIWindow *win);

/* 成功装好返回 1，没装成返回 0（调用方可以过一会儿再试）。
   ⚠ 关键：dylib 加载时 App 往往【还没有 UIWindow】，一次失败就永远没按钮了。
   所以安装必须能反复重试，直到真的装上。 */
static int  g_installed = 0;
static char g_ctl_key = 0;   /* associatedObject 的 key（用地址，不用字符串字面量） */

static UIWindow *find_window(void)
{
    UIApplication *app = [UIApplication sharedApplication];
    if (!app) return nil;
    /* iOS 13+ 多场景：先找前台活跃的窗口 */
    for (UIScene *sc in app.connectedScenes) {
        if ([sc isKindOfClass:[UIWindowScene class]] &&
            sc.activationState == UISceneActivationStateForegroundActive) {
            for (UIWindow *w in ((UIWindowScene *)sc).windows) if (w) return w;
        }
    }
    /* 兜底 1：任意场景里的任意窗口 */
    for (UIScene *sc in app.connectedScenes) {
        if ([sc isKindOfClass:[UIWindowScene class]])
            for (UIWindow *w in ((UIWindowScene *)sc).windows) if (w) return w;
    }
    /* 兜底 2：老 API */
    if ([app respondsToSelector:@selector(keyWindow)]) return app.keyWindow;
    return nil;
}

int nebula_ui_install_now(void)
{
    if (g_installed) return 1;

    __block int ok = 0;
    void (^blk)(void) = ^{
        @try {
            UIWindow *win = find_window();
            if (!win) return;
            nebula_ui_init([docs_dir() UTF8String]);
            build_panel(win);

            UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
            btn.frame = CGRectMake(win.bounds.size.width - 100.0, 96.0, 88.0, 36.0);
            [btn setTitle:@"职业" forState:UIControlStateNormal];
            btn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
            btn.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.5];
            btn.layer.cornerRadius = 10;
            btn.layer.borderWidth = 1;
            btn.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.6].CGColor;
            btn.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                                   UIViewAutoresizingFlexibleBottomMargin;
            NebulaPanelCtl *ctl = objc_getAssociatedObject(g_panel, &g_ctl_key);
            [btn addTarget:ctl action:@selector(onToggle:) forControlEvents:UIControlEventTouchUpInside];
            [win addSubview:btn];
            [win bringSubviewToFront:btn];
            ok = 1;
        } @catch (NSException *e) {
            nebula_log("面板：创建异常 %s", [[e reason] UTF8String]);
        }
    };

    if ([NSThread isMainThread]) blk();
    else dispatch_sync(dispatch_get_main_queue(), blk);

    if (ok) {
        g_installed = 1;
        nebula_log("面板：已创建（右上角悬浮按钮「职业」）");
    }
    return ok;
}

static NSString *docs_dir(void)
{
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                         NSUserDomainMask, YES);
    return paths.count ? paths[0] : @"";
}

static void build_panel(UIWindow *win)
{
    CGFloat W = win.bounds.size.width, H = win.bounds.size.height;
    CGFloat pw = 340.0, px = W - pw - 8.0, py = 90.0, ph = H - py - 40.0;

    UIView *panel = [[UIView alloc] initWithFrame:CGRectMake(px, py, pw, ph)];
    panel.backgroundColor = [UIColor colorWithWhite:0.05 alpha:0.92];
    panel.layer.cornerRadius = 12;
    panel.layer.borderWidth = 1;
    panel.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.25].CGColor;
    panel.hidden = YES;
    panel.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                             UIViewAutoresizingFlexibleHeight;

    NebulaPanelCtl *ctl = [NebulaPanelCtl new];

    /* ---- 工具条 ---- */
    UIView *bar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, pw, 96)];
    bar.backgroundColor = [UIColor colorWithWhite:0.15 alpha:1.0];

    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(12, 8, 220, 22)];
    title.text = [NSString stringWithFormat:@"职业设置（%d 个）", nebula_ui_role_count()];
    title.textColor = [UIColor whiteColor];
    title.font = [UIFont boldSystemFontOfSize:14];
    [bar addSubview:title];

    UISwitch *fp = [[UISwitch alloc] initWithFrame:CGRectMake(12, 36, 51, 31)];
    fp.on = (nebula_ui_freeplay_on() != 0);
    [fp addTarget:ctl action:@selector(onFreeplay:) forControlEvents:UIControlEventValueChanged];
    [bar addSubview:fp];

    UILabel *fpLb = [[UILabel alloc] initWithFrame:CGRectMake(70, 36, 84, 31)];
    fpLb.text = @"自由模式";
    fpLb.textColor = [UIColor whiteColor];
    fpLb.font = [UIFont systemFontOfSize:13];
    [bar addSubview:fpLb];

    UIButton *nextBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    nextBtn.frame = CGRectMake(160, 36, 76, 31);
    [nextBtn setTitle:@"换职业" forState:UIControlStateNormal];
    nextBtn.layer.borderWidth = 1;
    nextBtn.layer.borderColor = [UIColor whiteColor].CGColor;
    nextBtn.layer.cornerRadius = 6;
    [nextBtn addTarget:ctl action:@selector(onNextRole:) forControlEvents:UIControlEventTouchUpInside];
    [bar addSubview:nextBtn];

    UIButton *closeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    closeBtn.frame = CGRectMake(pw - 72, 36, 60, 31);
    [closeBtn setTitle:@"收起" forState:UIControlStateNormal];
    [closeBtn addTarget:ctl action:@selector(onToggle:) forControlEvents:UIControlEventTouchUpInside];
    [bar addSubview:closeBtn];

    [panel addSubview:bar];

    /* ---- 267 行可滚动列表 ---- */
    UIScrollView *list = [[UIScrollView alloc] initWithFrame:CGRectMake(0, 96, pw, ph - 96)];
    CGFloat rowH = 40.0;
    int n = nebula_ui_role_count();
    list.contentSize = CGSizeMake(pw, rowH * n);
    g_weight_labels = [NSMutableArray arrayWithCapacity:n];

    for (int i = 0; i < n; i++) {
        UIView *row = [[UIView alloc] initWithFrame:CGRectMake(0, rowH * i, pw, rowH)];
        const char *nm = nebula_ui_role_name(i);

        UILabel *lb = [[UILabel alloc] initWithFrame:CGRectMake(12, 4, 150, 32)];
        lb.text = [NSString stringWithUTF8String:(nm ? nm : "")];
        lb.textColor = [UIColor whiteColor];
        lb.font = [UIFont systemFontOfSize:14];
        [row addSubview:lb];

        UILabel *wlb = [[UILabel alloc] initWithFrame:CGRectMake(168, 4, 34, 32)];
        int w = nebula_ui_role_weight(i);
        wlb.text = [NSString stringWithFormat:@"%d", w];
        wlb.textAlignment = NSTextAlignmentCenter;
        wlb.textColor = (w > 0) ? [UIColor systemGreenColor] : [UIColor lightGrayColor];
        wlb.font = [UIFont boldSystemFontOfSize:14];
        [row addSubview:wlb];
        [g_weight_labels addObject:wlb];

        UIButton *minus = [UIButton buttonWithType:UIButtonTypeSystem];
        minus.frame = CGRectMake(210, 4, 44, 32);
        [minus setTitle:@"-" forState:UIControlStateNormal];
        minus.tag = i;
        minus.layer.borderWidth = 1;
        minus.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.3].CGColor;
        [minus addTarget:ctl action:@selector(onMinus:) forControlEvents:UIControlEventTouchUpInside];
        [row addSubview:minus];

        UIButton *plus = [UIButton buttonWithType:UIButtonTypeSystem];
        plus.frame = CGRectMake(262, 4, 44, 32);
        [plus setTitle:@"+" forState:UIControlStateNormal];
        plus.tag = i;
        plus.layer.borderWidth = 1;
        plus.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.3].CGColor;
        [plus addTarget:ctl action:@selector(onPlus:) forControlEvents:UIControlEventTouchUpInside];
        [row addSubview:plus];

        [list addSubview:row];
    }
    [panel addSubview:list];

    objc_setAssociatedObject(panel, &g_ctl_key, ctl, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    g_list  = list;
    g_panel = panel;
    [win addSubview:panel];
}

__attribute__((constructor))
static void nebula_ui_boot(void)
{
    /* 这里常常拿不到窗口（App 还没起来），失败也没关系 ——
       bridge 侧会持续重试 nebula_ui_install_now()。 */
    nebula_ui_install_now();
}

#endif /* __APPLE__ */
