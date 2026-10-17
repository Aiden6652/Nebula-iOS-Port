/*
 * dispatch.c — 网络层收发包分发实现
 * 把 protocol.c 的编解码接到"注册表 + transport"上，供 dylib 直接调用。
 */

#include "net_layer.h"
#include "protocol.h"
#include <string.h>

#define MAX_HANDLERS 64

static struct {
    nebula_rpc_t          id;
    nebula_rpc_handler_t  handler;
} g_handlers[MAX_HANDLERS];

static int g_nh = 0;
static int (*g_transport_send)(const uint8_t *, size_t) = NULL;

void nebula_net_register(nebula_rpc_t id, nebula_rpc_handler_t handler)
{
    for (int i = 0; i < g_nh; i++) {
        if (g_handlers[i].id == id) { g_handlers[i].handler = handler; return; }
    }
    if (g_nh < MAX_HANDLERS) {
        g_handlers[g_nh].id = id;
        g_handlers[g_nh].handler = handler;
        g_nh++;
    }
}

void nebula_net_set_transport(int (*sendfn)(const uint8_t *buf, size_t len))
{
    g_transport_send = sendfn;
}

int nebula_net_send(const uint8_t *buf, size_t len)
{
    if (!g_transport_send) return -1;
    return g_transport_send(buf, len);
}

void nebula_net_on_recv(const uint8_t *buf, size_t len)
{
    nebula_rpc_t rpc;
    const uint8_t *payload;
    uint16_t plen;

    /* 解码失败（魔数/版本/长度不符）直接丢弃，不路由 */
    if (nebula_msg_decode(buf, len, &rpc, &payload, &plen) != 0) return;

    for (int i = 0; i < g_nh; i++) {
        if (g_handlers[i].id == rpc && g_handlers[i].handler) {
            g_handlers[i].handler(payload, plen);
            return;
        }
    }
    /* 未注册 RPC：忽略（兼容未来增量） */
}
