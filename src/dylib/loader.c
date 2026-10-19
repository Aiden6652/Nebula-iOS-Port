/*
 * nebula_ios_loader.c
 * 自建 iOS 模组加载器（替代 BepInEx 的简易框架）
 *
 * 说明：
 *  - 本文件为【可编译空壳】，目标为 arm64-apple-ios。
 *  - 真实逻辑（IL2CPP 桥接、Hook 注册、玩法重写）在 dump 到方法地址后填充。
 *  - 日志写到 stderr；设备上可改用 syslog / 文件以便调试。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../net/net_layer.h"
#include "../net/protocol.h"
#include "../core/core.h"
#include "../addon/addon.h"
#include "../il2cpp/bridge.h"
#include "../il2cpp/hook.h"

/* ===== 简易日志 ===== */
static void port_log(const char *msg) {
    if (msg) {
        fputs("[Nebula-iOS] ", stderr);
        fputs(msg, stderr);
        fputc('\n', stderr);
    }
}

/* ===== IL2CPP 桥接上下文（占位，运行时由 Frida 解析地址填入） ===== */
typedef struct {
    void *domain;   /* Il2CppDomain */
    void *image;    /* Assembly-CSharp 镜像 */
} il2cpp_ctx_t;

static il2cpp_ctx_t g_ctx = {0};

/* ===== 本地玩法内核（纯本地逻辑，联机时只广播动作） ===== */
static nebula_game_t *g_game = NULL;

/* 本地动作回调：本地发起的技能 → 编码成 RPC_ABILITY_USE → 走网络层广播 */
static void on_local_action(uint32_t caster, uint16_t ability,
                            uint32_t target, float x, float y) {
    uint8_t pl[32]; uint16_t pln;
    if (nebula_pack_ability(caster, ability, target, x, y, pl, &pln) == 0) {
        uint8_t msg[64]; size_t mlen;
        if (nebula_msg_encode(RPC_ABILITY_USE, pl, pln, msg, &mlen) == 0) {
            nebula_net_send(msg, mlen);
        }
    }
}

/* 收到对端的技能动作 → 本地用同一套逻辑重算（不二次广播） */
static void on_ability_recv(const uint8_t *payload, size_t len) {
    uint32_t caster, target; uint16_t ability; float x, y;
    if (nebula_unpack_ability(payload, (uint16_t)len,
                              &caster, &ability, &target, &x, &y) == 0) {
        nebula_core_use_ability(g_game, caster, ability, target, x, y, 0);
    }
}

/* ===== 自洽网络层：示例 RPC 处理器 =====
 * 真实玩法会为每类自定义消息注册处理器（角色/技能/胜负…）。
 * 这里先注册一个 ROLE_ASSIGN 处理器，证明 dylib 已接入网络层。 */
static void on_role_assign(const uint8_t *payload, size_t len) {
    uint32_t net_id; uint16_t role_id;
    if (nebula_unpack_role(payload, (uint16_t)len, &net_id, &role_id) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "recv ROLE_ASSIGN net_id=%u role=%u", net_id, role_id);
        port_log(buf);
    }
}

/* ===== 底层发送通道（占位） =====
 * 真实情况下由 dylib 在 Hook 原版 Among Us 网络发送点时接入：
 * 把游戏的网络发送函数包一层，自定义消息从这里出门。 */
static int transport_send_stub(const uint8_t *buf, size_t len) {
    (void)buf; (void)len;
    return 0; /* TODO: 转发到原版发送点 */
}

/* ===== 初始化 IL2CPP 桥接（占位） ===== */
void nebula_port_init(void) {
    port_log("loader constructed");
    /* TODO: 用 frida-il2cpp-bridge 获取 Il2CppDomain / Assembly-CSharp 镜像并填入 g_ctx */
}

/* ===== 注册 native Hook（占位） ===== */
void nebula_port_hook_setup(void) {
    port_log("hook setup placeholder");
    /* TODO: 用 Frida Interceptor 在 dump 出的目标方法原生地址上打补丁
     *       例如 PlayerControl.FixedUpdate / GameManager.Update */
}

/* ===== 自洽网络层 + 本地内核 ===== */
void nebula_port_net_init(void) {
    g_game = nebula_core_default_game();
    nebula_core_set_action_cb(on_local_action);   /* 本地动作 → 广播 */
    nebula_net_set_transport(transport_send_stub);
    nebula_net_register(RPC_ROLE_ASSIGN, on_role_assign);
    nebula_net_register(RPC_ABILITY_USE, on_ability_recv);  /* 对端动作 → 本地重算 */
    port_log("net layer + local core initialized");
}

/* ===== 供 Frida/JS 桥接：返回本地游戏实例指针 ===== */
nebula_game_t *nebula_get_game(void) { return g_game; }

/* ===== 无 Frida 的游戏桥接：dylib 自己解析 libil2cpp 并轮询生效 =====
 * 不再依赖 Frida Gadget（用户环境没有），也不需要越狱；
 * 只读/写游戏对象字段与调用原版方法，不需要 JIT。 */
/* ===== 每帧回调：由 Update hook 调用，驱动本地引擎推进 ===== */
static void nebula_port_tick(void) {
    nebula_core_tick(g_game);
}

/* 无 Frida 的游戏桥接：dylib 自己解析 libil2cpp 并轮询生效 */
void nebula_port_bridge_start(void) {
    /* ★ 注意：绝不能在 constructor 里调用任何 IL2CPP API ——
     *   此刻 Unity 还没初始化 il2cpp，domain 是空的，一调就闪退。
     *   真正的 dump / hook 全部推迟到轮询线程里、等 IL2CPP 就绪后再做。 */
    /* 逐级排查开关：放 nebula_no_bridge.txt 可完全不启动游戏桥
       （不碰 IL2CPP、不装 hook），用来判断闪退是否来自桥接本身 */
    if (nebula_flag_file("nebula_no_bridge.txt")) {
        port_log("bridge 已停用（检测到 nebula_no_bridge.txt）");
        return;
    }
    /* 注册每帧要做的事：Update hook 成功后会每帧调用它 */
    nebula_hook_set_tick(nebula_port_tick);
    /* 启动后台轮询；其内部的poll线程在 IL2CPP 就绪后会自动安装 Update hook */
    nebula_il2cpp_start_poll(500);
}

/* ===== 原版插件（Addon）读取 =====
 * 目录：<App Documents>/addons/ —— 开启文件共享后可在电脑上直接丢插件进去。
 * 支持 ZIP 包 / 文件夹 / 原生 dylib；原样丢进去即可，不用解压。 */
void nebula_port_addon_init(void) {
    const char *home = getenv("HOME");
    int n = nebula_addon_init(home);
    char buf[128];
    snprintf(buf, sizeof(buf), "addons 已加载 %d 个插件%s", n, home ? "" : " (HOME 不可用)");
    port_log(buf);
    /* 抽一个真实词条验证插件生效（便于日志确认） */
    snprintf(buf, sizeof(buf), "样例词条 title.buttons.nebula = %s",
             nebula_lang_get("title.buttons.nebula"));
    port_log(buf);
}

/* ===== dylib 入口：dyld 加载阶段自动执行 ===== */
__attribute__((constructor))
static void loader_entry(void) {
    nebula_port_init();
    nebula_port_net_init();
    nebula_port_addon_init();
    nebula_port_hook_setup();
    nebula_port_bridge_start();
    port_log("dylib loaded");
}
