/*
 * protocol.h — Nebula-iOS 自洽网络协议（iOS ↔ iOS，不绑定 PC Reactor）
 *
 * 这是整个联机部分的"真相之源"：C 端（dylib 内）和 Python 参考实现
 * 必须严格遵守同一套线格式与 RPC 语义，两端才能互相同步。
 *
 * 线格式（小端 / little-endian）：
 *   +--------+--------+--------+--------+----------+
 *   | magic  | ver    | rpc_id | plen   | payload  |
 *   | 2B 'NB'| 1B     | 2B u16 | 2B u16 | plen B   |
 *   +--------+--------+--------+--------+----------+
 *   头部固定 7 字节；payload 由具体 RPC 决定。
 */

#ifndef NEBULA_PROTOCOL_H
#define NEBULA_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* 协议魔数与版本（两端必须一致，否则直接丢弃） */
#define NEBULA_MAGIC        0x4E42u   /* 'N''B' */
#define NEBULA_PROTO_VERSION 0x01u

/* 头部长度（magic2 + ver1 + rpc_id2 + plen2） */
#define NEBULA_HDR_LEN      7u

/* 单个消息最大长度（防御畸形包） */
#define NEBULA_MAX_MSG      4096u

/*
 * 自定义 RPC 清单（自洽定义，不与 PC 协议兼容）。
 * 所有消息 ID 从 0x5000 起，避开原版 Among Us 占用区间。
 */
typedef enum {
    RPC_HELLO       = 0x5000,  /* 握手：携带协议 epoch，两端校验 */
    RPC_ROLE_ASSIGN = 0x5001,  /* 角色分配：net_id -> role_id */
    RPC_ABILITY_USE = 0x5002,  /* 技能释放：caster/ability/target/(x,y) */
    RPC_WIN_CHECK   = 0x5003,  /* 胜负判定：winner_team */
    RPC_STATE_SYNC  = 0x5004,  /* 通用状态同步：net_id/key/val */
} nebula_rpc_t;

/*
 * 把一条消息编码进 out（至少 NEBULA_MAX_MSG 字节）。
 * 成功返回 0，*out_len 为总字节数；失败返回 -1。
 */
int nebula_msg_encode(nebula_rpc_t rpc,
                      const uint8_t *payload, uint16_t plen,
                      uint8_t *out, size_t *out_len);

/*
 * 从 buf/len 解码一条消息。
 * 成功返回 0，填充 *rpc / *payload / *plen（payload 指向 buf 内部，勿释放）。
 * 魔数/版本不符或长度越界返回 -1。
 */
int nebula_msg_decode(const uint8_t *buf, size_t len,
                      nebula_rpc_t *rpc,
                      const uint8_t **payload, uint16_t *plen);

/* 各 RPC 的 payload 打包/解包辅助（小端，与 Python 参考实现一致） */

/* HELLO: 4B epoch(u32) */
int nebula_pack_hello(uint32_t epoch, uint8_t *out, uint16_t *out_len);
int nebula_unpack_hello(const uint8_t *p, uint16_t n, uint32_t *epoch);

/* ROLE_ASSIGN: net_id(u32) + role_id(u16) */
int nebula_pack_role(uint32_t net_id, uint16_t role_id,
                     uint8_t *out, uint16_t *out_len);
int nebula_unpack_role(const uint8_t *p, uint16_t n,
                       uint32_t *net_id, uint16_t *role_id);

/* ABILITY_USE: caster(u32)+ability(u16)+target(u32)+x(f32)+y(f32) */
int nebula_pack_ability(uint32_t caster, uint16_t ability,
                        uint32_t target, float x, float y,
                        uint8_t *out, uint16_t *out_len);
int nebula_unpack_ability(const uint8_t *p, uint16_t n,
                          uint32_t *caster, uint16_t *ability,
                          uint32_t *target, float *x, float *y);

/* WIN_CHECK: winner_team(u8) */
int nebula_pack_win(uint8_t winner_team, uint8_t *out, uint16_t *out_len);
int nebula_unpack_win(const uint8_t *p, uint16_t n, uint8_t *winner_team);

/* STATE_SYNC: net_id(u32)+key(u8)+val(i32) */
int nebula_pack_state(uint32_t net_id, uint8_t key, int32_t val,
                      uint8_t *out, uint16_t *out_len);
int nebula_unpack_state(const uint8_t *p, uint16_t n,
                        uint32_t *net_id, uint8_t *key, int32_t *val);

#endif /* NEBULA_PROTOCOL_H */
