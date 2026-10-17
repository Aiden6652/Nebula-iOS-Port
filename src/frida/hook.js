/*
 * hook.js — 第一个真实 Hook 的脚手架（在 dump 出地址后填充）
 *
 * 用法：frida -U -l hook.js -f com.innersloth.amongus
 *
 * 说明：
 *  - 一律用 frida-il2cpp-bridge 的 Il2Cpp 接口拿到方法对象后 attach，
 *    Frida 会自动处理 W^X 与 ARM64 PAC，不要手搓 inline hook。
 *  - 下面的 PlayerControl.FixedUpdate 是示例占位，请用 dump.js 的结果替换/确认。
 */

const { Il2Cpp } = require("frida-il2cpp-bridge");

async function main() {
  await Il2Cpp.perform(() => {
    const image = Il2Cpp.domain.assembly("Assembly-CSharp").image;

    // 示例：Hook PlayerControl.FixedUpdate（移动/技能逻辑入口）
    const playerControl = image.class("PlayerControl");
    if (playerControl) {
      for (const m of playerControl.methods) {
        if (m.name === "FixedUpdate") {
          m.implementation = function (...args) {
            // TODO: 在此插入重写的星云舰逻辑（例如修改速度/触发技能）
            // console.log("[Nebula] PlayerControl.FixedUpdate called");
            return this.methodInvocation(...args);
          };
          console.log("[+] Hooked PlayerControl.FixedUpdate @", m.relativePointer);
        }
      }
    }

    // 示例：Hook GameManager.Update（回合/胜负判定入口）
    const gameManager = image.class("GameManager");
    if (gameManager) {
      for (const m of gameManager.methods) {
        if (m.name === "Update") {
          m.implementation = function (...args) {
            // TODO: 在此插入重写的星云舰回合/胜负逻辑
            return this.methodInvocation(...args);
          };
          console.log("[+] Hooked GameManager.Update @", m.relativePointer);
        }
      }
    }

    console.log("[*] hook 脚手架已注册（占位）。替换 TODO 后即第一个 PoC。");
  });
}

main().catch((e) => console.error("[!] 致命错误:", e));
