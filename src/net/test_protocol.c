/*
 * test_protocol.c — host 端协议往返测试（非 iOS，纯 gcc 验证编解码正确性）
 * 跑法：cd src/net && make test
 */

#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if(!(cond)){ printf("  [FAIL] %s\n", msg); failures++; } \
    else { printf("  [ok]   %s\n", msg); } \
} while(0)

static void test_roundtrip(nebula_rpc_t rpc,
                           const uint8_t *payload, uint16_t plen)
{
    uint8_t msg[NEBULA_MAX_MSG]; size_t mlen;
    int rc = nebula_msg_encode(rpc, payload, plen, msg, &mlen);
    CHECK(rc == 0, "encode 返回 0");
    CHECK(mlen == NEBULA_HDR_LEN + plen, "总长度 = 头 + payload");

    nebula_rpc_t drpc; const uint8_t *dp; uint16_t dplen;
    rc = nebula_msg_decode(msg, mlen, &drpc, &dp, &dplen);
    CHECK(rc == 0, "decode 返回 0");
    CHECK(drpc == rpc, "rpc_id 一致");
    CHECK(dplen == plen, "payload 长度一致");
    CHECK(memcmp(dp, payload, plen) == 0, "payload 字节一致");
}

int main(void)
{
    printf("=== Nebula protocol host test ===\n");

    /* 各 RPC 打包后做编解码往返 */
    uint8_t b[32]; uint16_t n;

    nebula_pack_hello(0xDEADBEEF, b, &n); test_roundtrip(RPC_HELLO, b, n);

    nebula_pack_role(123456, 7, b, &n); test_roundtrip(RPC_ROLE_ASSIGN, b, n);

    nebula_pack_ability(999, 3, 888, 12.5f, -3.25f, b, &n);
    test_roundtrip(RPC_ABILITY_USE, b, n);

    nebula_pack_win(2, b, &n); test_roundtrip(RPC_WIN_CHECK, b, n);

    nebula_pack_state(42, 5, -100, b, &n); test_roundtrip(RPC_STATE_SYNC, b, n);

    /* 解包字段正确性抽查 */
    uint32_t net_id; uint16_t role_id;
    nebula_pack_role(123456, 7, b, &n);
    nebula_unpack_role(b, n, &net_id, &role_id);
    CHECK(net_id == 123456 && role_id == 7, "ROLE 字段解包正确");

    uint32_t caster, target; uint16_t ability; float x, y;
    nebula_pack_ability(999, 3, 888, 12.5f, -3.25f, b, &n);
    nebula_unpack_ability(b, n, &caster, &ability, &target, &x, &y);
    CHECK(caster==999 && ability==3 && target==888
          && x==12.5f && y==-3.25f, "ABILITY 字段解包正确(含浮点)");

    /* 畸形包必须被拒 */
    uint8_t bad[3] = {0,0,0};
    nebula_rpc_t r; const uint8_t *p; uint16_t pl;
    CHECK(nebula_msg_decode(bad, 3, &r, &p, &pl) == -1, "短包被拒");
    uint8_t wrong_magic[7] = {0,0,1,0,0,0,0};
    CHECK(nebula_msg_decode(wrong_magic, 7, &r, &p, &pl) == -1, "错误魔数被拒");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
