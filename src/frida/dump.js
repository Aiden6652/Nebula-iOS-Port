/*
 * dump.js — 用 frida-il2cpp-bridge 在运行中的 Among Us(iOS, 已开 JIT) 中
 *           dump IL2CPP 类 / 方法 / 字段，定位需要 Hook 的 C# 函数原生地址。
 *
 * 用法（设备端已注入 Frida Gadget 且开 JIT）：
 *   frida -U -l dump.js -f com.innersloth.amongus
 * 或把本脚本作为 Gadget 的配套脚本随 dylib 一起加载。
 */

const { Il2Cpp, Il2CppClass } = require("frida-il2cpp-bridge");

async function main() {
  await Il2Cpp.perform(() => {
    const assembly = Il2Cpp.domain.assembly("Assembly-CSharp");
    const image = assembly.image;

    // 示例：列出 PlayerControl 的方法（定位 FixedUpdate 等 Hook 点）
    const playerControl = image.class("PlayerControl");
    if (playerControl) {
      console.log("[*] PlayerControl methods:");
      for (const m of playerControl.methods) {
        console.log("   ", m.name, "->", m.relativePointer);
      }
    }

    // 示例：列出 GameManager 的方法（定位 Update / 回合逻辑）
    const gameManager = image.class("GameManager");
    if (gameManager) {
      console.log("[*] GameManager methods:");
      for (const m of gameManager.methods) {
        console.log("   ", m.name, "->", m.relativePointer);
      }
    }

    // TODO: 全量 dump 到文件，或按需搜索目标类/方法。
  });
}

main().catch((e) => console.error(e));
