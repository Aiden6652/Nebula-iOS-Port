/*
 * protocol.c — Nebula-iOS 自洽网络协议实现
 * 与 src/net/protocol.h 配套；Python 参考实现（tools/sim/protocol_sim.py）
 * 必须产生/解析完全相同的字节流。
 */

#include "protocol.h"
#include <string.h>

/* ---------- 基础编解码 ---------- */

int nebula_msg_encode(nebula_rpc_t rpc,
                      const uint8_t *payload, uint16_t plen,
                      uint8_t *out, size_t *out_len)
{
    if (!out || !out_len) return -1;
    if (plen > NEBULA_MAX_MSG - NEBULA_HDR_LEN) return -1;

    uint16_t rpc_u = (uint16_t)rpc;
    out[0] = (uint8_t)(NEBULA_MAGIC & 0xFF);          /* 'N' */
    out[1] = (uint8_t)((NEBULA_MAGIC >> 8) & 0xFF);   /* 'B' */
    out[2] = (uint8_t)NEBULA_PROTO_VERSION;
    out[3] = (uint8_t)(rpc_u & 0xFF);
    out[4] = (uint8_t)((rpc_u >> 8) & 0xFF);
    out[5] = (uint8_t)(plen & 0xFF);
    out[6] = (uint8_t)((plen >> 8) & 0xFF);
    if (plen && payload) memcpy(out + NEBULA_HDR_LEN, payload, plen);

    *out_len = NEBULA_HDR_LEN + plen;
    return 0;
}

int nebula_msg_decode(const uint8_t *buf, size_t len,
                      nebula_rpc_t *rpc,
                      const uint8_t **payload, uint16_t *plen)
{
    if (!buf || len < NEBULA_HDR_LEN) return -1;

    uint16_t magic = (uint16_t)(buf[0] | (buf[1] << 8));
    if (magic != NEBULA_MAGIC) return -1;
    if (buf[2] != NEBULA_PROTO_VERSION) return -1;

    uint16_t rpc_u = (uint16_t)(buf[3] | (buf[4] << 8));
    uint16_t pl    = (uint16_t)(buf[5] | (buf[6] << 8));
    if ((size_t)NEBULA_HDR_LEN + pl > len) return -1;

    if (rpc)    *rpc = (nebula_rpc_t)rpc_u;
    if (plen)   *plen = pl;
    if (payload)*payload = (pl > 0) ? (buf + NEBULA_HDR_LEN) : NULL;
    return 0;
}

/* ---------- 小端读写辅助 ---------- */

static void put_u16(uint8_t *b, uint16_t v){ b[0]=(uint8_t)(v&0xFF); b[1]=(uint8_t)((v>>8)&0xFF); }
static void put_u32(uint8_t *b, uint32_t v){ b[0]=(uint8_t)(v&0xFF); b[1]=(uint8_t)((v>>8)&0xFF); b[2]=(uint8_t)((v>>16)&0xFF); b[3]=(uint8_t)((v>>24)&0xFF); }
static void put_i32(uint8_t *b, int32_t v){ put_u32(b, (uint32_t)v); }
static void put_f32(uint8_t *b, float v){ uint32_t bits; memcpy(&bits,&v,4); put_u32(b,bits); }

static uint16_t get_u16(const uint8_t *b){ return (uint16_t)(b[0] | (b[1]<<8)); }
static uint32_t get_u32(const uint8_t *b){ return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24); }
static int32_t  get_i32(const uint8_t *b){ return (int32_t)get_u32(b); }
static float    get_f32(const uint8_t *b){ uint32_t bits=get_u32(b); float v; memcpy(&v,&bits,4); return v; }

/* ---------- 各 RPC 打包/解包 ---------- */

int nebula_pack_hello(uint32_t epoch, uint8_t *out, uint16_t *out_len){
    if(!out||!out_len) return -1;
    put_u32(out, epoch); *out_len = 4; return 0;
}
int nebula_unpack_hello(const uint8_t *p, uint16_t n, uint32_t *epoch){
    if(!p || n < 4 || !epoch) return -1;
    *epoch = get_u32(p); return 0;
}

int nebula_pack_role(uint32_t net_id, uint16_t role_id, uint8_t *out, uint16_t *out_len){
    if(!out||!out_len) return -1;
    put_u32(out, net_id); put_u16(out+4, role_id); *out_len = 6; return 0;
}
int nebula_unpack_role(const uint8_t *p, uint16_t n, uint32_t *net_id, uint16_t *role_id){
    if(!p || n < 6 || !net_id || !role_id) return -1;
    *net_id = get_u32(p); *role_id = get_u16(p+4); return 0;
}

int nebula_pack_ability(uint32_t caster, uint16_t ability, uint32_t target,
                        float x, float y, uint8_t *out, uint16_t *out_len){
    if(!out||!out_len) return -1;
    put_u32(out, caster); put_u16(out+4, ability); put_u32(out+6, target);
    put_f32(out+10, x); put_f32(out+14, y); *out_len = 18; return 0;
}
int nebula_unpack_ability(const uint8_t *p, uint16_t n,
                          uint32_t *caster, uint16_t *ability,
                          uint32_t *target, float *x, float *y){
    if(!p || n < 18 || !caster||!ability||!target||!x||!y) return -1;
    *caster = get_u32(p); *ability = get_u16(p+4); *target = get_u32(p+6);
    *x = get_f32(p+10); *y = get_f32(p+14); return 0;
}

int nebula_pack_win(uint8_t winner_team, uint8_t *out, uint16_t *out_len){
    if(!out||!out_len) return -1;
    out[0] = winner_team; *out_len = 1; return 0;
}
int nebula_unpack_win(const uint8_t *p, uint16_t n, uint8_t *winner_team){
    if(!p || n < 1 || !winner_team) return -1;
    *winner_team = p[0]; return 0;
}

int nebula_pack_state(uint32_t net_id, uint8_t key, int32_t val,
                      uint8_t *out, uint16_t *out_len){
    if(!out||!out_len) return -1;
    put_u32(out, net_id); out[4] = key; put_i32(out+5, val); *out_len = 9; return 0;
}
int nebula_unpack_state(const uint8_t *p, uint16_t n,
                        uint32_t *net_id, uint8_t *key, int32_t *val){
    if(!p || n < 9 || !net_id || !key || !val) return -1;
    *net_id = get_u32(p); *key = p[4]; *val = get_i32(p+5); return 0;
}
