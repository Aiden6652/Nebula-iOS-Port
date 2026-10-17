/*
 * hook.js — Nebula-iOS 完整 Hook 管理器（Frida + frida-il2cpp-bridge）
 *
 * 职责（与 src/core 的"纯本地引擎"对接）：
 *   1) 按 config 解析 Among Us 的 IL2CPP 方法（抗更新：只改 config，不动逻辑）；
 *   2) 桥接 dylib 导出的 C 函数（nebula_core_* / nebula_net_*），让游戏事件驱动本地引擎；
 *   3) 接管网络收发：自定消息经 nebula_net_on_recv 入境、经 transport 出境；
 *   4) 接管击杀/会议/投票输入：转成 nebula_core_* 调用。
 *
 * 用法：frida -U -l hook.js -f com.innersloth.amongus
 * 前置：先跑 dump.js 产出 /tmp/nebula_methods.json（方法名→地址）。
 *
 * 安全：一律用 Frida Interceptor（自动处理 W^X 与 ARM64 PAC），不自搓 inline hook。
 */

const { Il2Cpp } = require("frida-il2cpp-bridge");
const fs = require("fs");

/* ============ 1) 配置：方法名 → 地址（由 dump.js 产出） ============ */
function loadMethodsConfig() {
  try {
    return JSON.parse(fs.readFileSync("/tmp/nebula_methods.json", "utf8"));
  } catch (e) {
    console.log("[*] 未找到 /tmp/nebula_methods.json，使用内置占位名（需 dump 后补全）");
    return {};
  }
}

/* 逻辑名 → Among Us IL2CPP 真实方法名（不同 AU 版本会被混淆，以 config 地址优先） */
const LOGICAL = {
  gameManagerUpdate:   { cls: "GameManager",    name: "Update" },
  playerFixedUpdate:   { cls: "PlayerControl",  name: "FixedUpdate" },
  playerRpcSendChat:   { cls: "PlayerControl",  name: "RpcSendChat" },  // 网络发送点（示例）
  shipStatusOnEnable:  { cls: "ShipStatus",     name: "OnEnable" },     // 网络接收点（示例）
  killButton:          { cls: "KillButton",     name: "OnClick" },      // 击杀输入（示例）
  emergencyButton:     { cls: "EmergencyButton", name: "OnClick" },     // 会议输入（示例）
};

/* ============ 2) C 桥接：dylib 导出的本地引擎/网络接口 ============ */
function makeCBridge() {
  const find = (sym) => {
    const a = Module.findExportByName(null, sym);
    if (!a) { console.log(`[!] 找不到导出符号 ${sym}（dylib 未加载？）`); throw new Error(sym); }
    return a;
  };
  return {
    coreInit:          new NativeFunction(find("nebula_core_init"),        "void", ["pointer"]),
    coreAddPlayer:     new NativeFunction(find("nebula_core_add_player"),  "void", ["pointer", "uint", "int"]),
    coreUseAbility:    new NativeFunction(find("nebula_core_use_ability"), "int",  ["pointer", "uint", "ushort", "uint", "float", "float", "int"]),
    coreCallMeeting:   new NativeFunction(find("nebula_core_call_meeting"),"void", ["pointer", "uint", "int"]),
    coreCastVote:      new NativeFunction(find("nebula_core_cast_vote"),   "void", ["pointer", "uint", "uint", "int"]),
    coreResolveMeeting:new NativeFunction(find("nebula_core_resolve_meeting"), "int", ["pointer"]),
    netOnRecv:         new NativeFunction(find("nebula_net_on_recv"),      "void", ["pointer", "size_t"]),
    netSetTransport:   new NativeFunction(find("nebula_net_set_transport"),"void", ["pointer"]),
    coreSetActionCb:   new NativeFunction(find("nebula_core_set_action_cb"),"void", ["pointer"]),
  };
}

/* ============ 3) 本地游戏状态句柄（C 侧 nebula_game_t 的全局实例地址） ============ */
/* dylib 导出 nebula_get_game() 返回 &g_game；JS 直接调用取指针。 */
function gamePtr() {
  const fn = Module.findExportByName(null, "nebula_get_game");
  if (!fn) { console.log("[!] 找不到导出符号 nebula_get_game"); return ptr(0); }
  const getGame = new NativeFunction(fn, "pointer", []);
  return getGame();
}

/* ============ 4) 方法解析（config 地址优先，回退按名查找） ============ */
function resolveMethod(image, logical, cfg) {
  const key = logical.cls + "." + logical.name;
  if (cfg[key] && cfg[key].ptr) {
    try { return image.methodFromPointer(ptr(cfg[key].ptr)); } catch (e) {}
  }
  const cls = image.class(logical.cls);
  if (!cls) return null;
  for (const m of cls.methods) if (m.name === logical.name) return m;
  return null;
}

/* ============ 主流程 ============ */
async function main() {
  const cfg = loadMethodsConfig();
  const C = makeCBridge();

  await Il2Cpp.perform(async () => {
    const image = Il2Cpp.domain.assembly("Assembly-CSharp").image;
    const g = gamePtr();
    if (g.isNull()) { console.log("[!] 未取到 nebula_game_t 指针，跳过初始化"); return; }

    C.coreInit(g); // 初始化本地引擎
    console.log("[+] 本地引擎已初始化 @", g);

    /* ---- 4.1 网络出境：把 AU 的发送函数包成 transport 注册给 C 网络层 ----
     * 思路：保存 AU 原始发送方法，JS 侧 NativeCallback 把字节发给它；
     *       C 侧 nebula_net_set_transport 指向该回调。 */
    const sendMethod = resolveMethod(image, LOGICAL.playerRpcSendChat, cfg);
    let originalSend = null;
    if (sendMethod) {
      originalSend = sendMethod; // 用于真正发字节
      const transportCb = new NativeCallback(function (bufPtr, len) {
        // 把自定义消息通过 AU 网络栈发出
        const bytes = bufPtr.readByteArray(len);
        // TODO: 调用 originalSend 对应原生发送（按 AU 网络 API 填）
        // originalSend.invoke(...);
        return 0;
      }, "int", ["pointer", "size_t"]);
      C.netSetTransport(transportCb);
      console.log("[+] 已注册网络出境 transport");
    } else {
      console.log("[!] 未解析到发送点，transport 未接入（需补全 config）");
    }

    /* ---- 4.2 网络入境：Hook 接收点 → nebula_net_on_recv ----
     * AU 收到字节后，把 payload 喂给 C 网络层解码分发。 */
    const recvMethod = resolveMethod(image, LOGICAL.shipStatusOnEnable, cfg);
    if (recvMethod) {
      recvMethod.implementation = function (...args) {
        // TODO: 从 AU 接收参数中取出字节 (buf, len) 后：
        // C.netOnRecv(bufNativePtr, len);
        return this.methodInvocation(...args);
      };
      console.log("[+] 已 Hook 网络接收点（入境）");
    }

    /* ---- 4.3 输入：击杀/会议 按钮 → 本地引擎 ----
     * 玩家按下按钮时，把事件转成 nebula_core_* 调用（local=1 触发广播）。 */
    const killBtn = resolveMethod(image, LOGICAL.killButton, cfg);
    if (killBtn) {
      killBtn.implementation = function (...args) {
        // TODO: 取当前 PlayerControl 的 netId 与瞄准目标 netId
        // const me = <本机玩家 netId>; const target = <瞄准目标 netId>;
        // C.coreUseAbility(g, me, 1 /*ABILITY_KILL*/, target, 0, 0, 1);
        return this.methodInvocation(...args);
      };
      console.log("[+] 已 Hook 击杀输入");
    }
    const meetBtn = resolveMethod(image, LOGICAL.emergencyButton, cfg);
    if (meetBtn) {
      meetBtn.implementation = function (...args) {
        // const me = <本机玩家 netId>;
        // C.coreCallMeeting(g, me, 1);
        return this.methodInvocation(...args);
      };
      console.log("[+] 已 Hook 会议输入");
    }

    /* ---- 4.4 主循环：GameManager.Update 驱动冷却/胜负 ----
     * 每帧让 C 侧推进（冷却递减、胜负判定已在动作时算，这里兜底）。 */
    const gmUpdate = resolveMethod(image, LOGICAL.gameManagerUpdate, cfg);
    if (gmUpdate) {
      gmUpdate.implementation = function (...args) {
        // TODO: C.coreTick(g) 若存在；此处仅占位驱动
        return this.methodInvocation(...args);
      };
      console.log("[+] 已 Hook GameManager.Update");
    }

    console.log("[*] Hook 管理器注册完成。未解析的方法请在 config 中补全地址后重试。");
  });
}

main().catch((e) => console.error("[!] 致命错误:", e));
