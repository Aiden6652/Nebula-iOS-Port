/*
 * net_layer.h
 * 自洽网络层接口（iOS ↔ iOS，不绑定 PC Reactor 协议）
 *
 * 设计要点：
 *  - 两端均为本移植版，RPC ID / 序列化 / 分发由本仓库定义，保证两份构建一致。
 *  - 底层通过 Hook 原版 Among Us 的网络收发点传输自定义消息。
 *  - 收包流程：游戏 recv 钩子 → nebula_net_on_recv → 解码 → 路由到注册的 RPC 处理器。
 *  - 发包流程：nebula_net_send → 注册的 transport（由 dylib 接入原版发送点）。
 *  - PC 兼容作为后续可选适配器，不影响本层。
 */

#ifndef NEBULA_NET_LAYER_H
#define NEBULA_NET_LAYER_H

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"   /* 复用 RPC 枚举与编解码 */

/* RPC 处理器签名：拿到解码后的 payload 与长度 */
typedef void (*nebula_rpc_handler_t)(const uint8_t *payload, size_t len);

/* 注册一个 RPC 的处理器（重复注册会覆盖） */
void nebula_net_register(nebula_rpc_t id, nebula_rpc_handler_t handler);

/*
 * 设置底层发送通道。dylib 在初始化时把"原版游戏的网络发送点"接进来。
 * 传 NULL 表示暂时不可用（nebula_net_send 会返回 -1）。
 */
void nebula_net_set_transport(int (*sendfn)(const uint8_t *buf, size_t len));

/* 发送已编码好的消息（内部调用 transport） */
int  nebula_net_send(const uint8_t *buf, size_t len);

/* 收包入口：由游戏 recv 钩子调用，内部解码并分发到对应处理器 */
void nebula_net_on_recv(const uint8_t *buf, size_t len);

#endif /* NEBULA_NET_LAYER_H */
