# 快速开始（不想看文档版）

> 命令都在**你的 Windows 电脑（MateBook）**上敲，iPad 只是被操作的目标。
> iPad 需：脱壳 Among Us IPA + 已注入 Frida Gadget + 已开 JIT（你已有的 LiveContainer/SideStore 链路）。

## 0. 电脑上一次性装环境

```powershell
pip install frida-tools
mkdir nebula && cd nebula
npm init -y
npm install frida-il2cpp-bridge
```

把仓库里的 `src/frida/dump.js` 和 `src/frida/hook.js` 放进这个文件夹。

## 1. 让 iPad 上的 Among Us 能被 Frida 连上

- 把 **Frida Gadget** dylib 和我们的 `nebula_ios_loader.dylib` 一起注入脱壳 IPA（见 `tools/inject.sh`），重签安装。
- 用 LiveContainer / Sideloadly 启动并**开 JIT**。
- iPad 用 USB 连电脑。

## 2. 跑 dump，拿到方法地址清单

```powershell
frida -U -l dump.js -f com.innersloth.amongus 2>&1 | tee nebula_dump.json
```

## 3. 解析出你要的 Hook 点

```powershell
python3 tools/parse_dump.py nebula_dump.json
python3 tools/parse_dump.py nebula_dump.json --grep Role
```

## 4. 写第一个 Hook

编辑 `hook.js` 里 `TODO` 处，把 dump 到的真实方法名/逻辑填进去，再跑：

```powershell
frida -U -l hook.js -f com.innersloth.amongus
```

看到 `[+] Hooked ...` 就说明链路通了——这就是 PoC 完成。

---
后续：把星云舰玩法逻辑用 C++ 重写，挂到这些 Hook 点上；网络同步走 `src/net/`。
