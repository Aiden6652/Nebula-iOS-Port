/*
 * net_layer.h
 * 自洽网络层接口（iOS ↔ iOS，不绑定 PC Reactor 协议）
 *
 * 设计要点：
 *  - 两端均为本移植版，RPC ID / 序列化 / 分发由本仓库定义，保证两份构建一致。
 *  - 底层通过 Hook 原版 Among Us 的网络收发点传输自定义消息。
 *  - PC 兼容作为后续可选适配器，不影响本层。
 */

#ifndef NEBULA_NET_LAYER_H
#define NEBULA_NET_LAYER_H

#include <stdint.h>
#include <stddef.h>

/* 自定义 RPC 起始 ID（避开原版 Among Us 占用区间） */
#define NEBULA_RPC_BASE 0x5000

typedef enum {
    RPC_ROLE_ASSIGN  = NEBULA_RPC_BASE + 1,  /* 角色分配 */
    RPC_ABILITY_USE  = NEBULA_RPC_BASE + 2,  /* 技能释放 */
    RPC_WIN_CHECK    = NEBULA_RPC_BASE + 3,  /* 胜负判定 */
    RPC_STATE_SYNC   = NEBULA_RPC_BASE + 4,  /* 状态同步 */
    /* TODO: 按 docs/PROTOCOL_NOTES.md 整理后补全 */
} nebula_rpc_t;

/* 发送自定义消息（底层 hook 原版收发点） */
int  nebula_net_send(const uint8_t *buf, size_t len);

/* 接收分发（由原版收包点回调） */
void nebula_net_on_recv(const uint8_t *buf, size_t len);

/* 注册 RPC 处理器 */
void nebula_net_register(nebula_rpc_t id, void (*handler)(const uint8_t *, size_t));

#endif /* NEBULA_NET_LAYER_H */
