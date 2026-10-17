/*
 * test_dispatch.c — host 端分发层测试
 * 验证：注册 RPC → 收包解码 → 路由到正确处理器；发送经 mock transport 出门。
 * 跑法：cd src/net && make test
 */

#include "net_layer.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

/* ---- mock transport：抓住发出去的字节 ---- */
static uint8_t g_sent[4096];
static size_t  g_sent_len = 0;
static int mock_send(const uint8_t *buf, size_t len){
    memcpy(g_sent, buf, len); g_sent_len = len; return 0;
}

/* ---- 处理器：记录被调用情况 ---- */
static int g_role_hits = 0;
static uint32_t g_role_net = 0;
static uint16_t g_role_rid = 0;
static void on_role(const uint8_t *p, size_t n){
    g_role_hits++;
    nebula_unpack_role(p, (uint16_t)n, &g_role_net, &g_role_rid);
}
static int g_ability_hits = 0;
static void on_ability(const uint8_t *p, size_t n){
    (void)p; (void)n; g_ability_hits++;
}
static int g_unknown_hits = 0;
static void on_hello(const uint8_t *p, size_t n){
    (void)p; (void)n; g_unknown_hits++;
}

int main(void)
{
    printf("=== Nebula dispatch host test ===\n");

    nebula_net_set_transport(mock_send);
    nebula_net_register(RPC_ROLE_ASSIGN, on_role);
    nebula_net_register(RPC_ABILITY_USE, on_ability);
    nebula_net_register(RPC_HELLO, on_hello);

    /* 构造一条 ROLE_ASSIGN 消息并喂给 on_recv */
    uint8_t pl[8]; uint16_t pln;
    nebula_pack_role(0xABCD, 7, pl, &pln);
    uint8_t msg[64]; size_t mlen;
    nebula_msg_encode(RPC_ROLE_ASSIGN, pl, pln, msg, &mlen);
    nebula_net_on_recv(msg, mlen);

    CHECK(g_role_hits == 1, "ROLE 处理器被调用 1 次");
    CHECK(g_role_net == 0xABCD && g_role_rid == 7, "ROLE 解包字段正确");

    /* 一条 ABILITY 消息 */
    uint8_t ap[32]; uint16_t an;
    nebula_pack_ability(1, 2, 3, 1.0f, 2.0f, ap, &an);
    uint8_t amsg[64]; size_t amlen;
    nebula_msg_encode(RPC_ABILITY_USE, ap, an, amsg, &amlen);
    nebula_net_on_recv(amsg, amlen);
    CHECK(g_ability_hits == 1, "ABILITY 处理器被调用 1 次");

    /* 非法包（错误魔数）应被丢弃，不触发任何处理器 */
    int before = g_role_hits + g_ability_hits + g_unknown_hits;
    uint8_t bad[7] = {0,0,1,0,0,0,0};
    nebula_net_on_recv(bad, 7);
    int after = g_role_hits + g_ability_hits + g_unknown_hits;
    CHECK(before == after, "非法包被丢弃，未触发处理器");

    /* 发送：经 transport 出门 */
    g_sent_len = 0;
    int rc = nebula_net_send(msg, mlen);
    CHECK(rc == 0 && g_sent_len == mlen, "nebula_net_send 经 transport 发出");

    /* transport 未设置时应返回 -1 */
    nebula_net_set_transport(NULL);
    CHECK(nebula_net_send(msg, mlen) == -1, "无 transport 时 send 返回 -1");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
