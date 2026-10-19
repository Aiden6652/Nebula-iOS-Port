# 闪退排查（逐级二分）

注入后如果 Among Us 一开就闪退，**不要靠猜**——用两个开关逐级二分定位。
开关用法：把对应文件放进 IPA 里、与可执行文件 `AmongUs` **同一目录**，重新签名安装。

## 默认行为（最小侵入）

不放任何开关时，dylib 只做一件事：**每 3 秒在游戏主线程给玩家名字加上职业**。
其余全部默认关闭：

| 功能 | 默认 | 开启方式 |
|---|---|---|
| 玩家名字显示职业 | ✅ 开 | — |
| **每帧 hook** | ❌ 关 | 放 `nebula_hook.txt` |
| 按职业调整速度 | ❌ 关 | 放 `nebula_speed.txt` |
| 身份界面文本替换 | ❌ 关 | 放 `nebula_role_text.txt` |

> 现成文件都在 `tools/flags/`，也可用
> `./tools/inject.sh game.ipa --flag nebula_hook.txt` 自动塞进 IPA。

## 逐级二分

| 放这个文件 | 效果 | 如果还异常说明 |
|---|---|---|
| `nebula_no_bridge.txt` | **完全不启动游戏桥**（不碰 IL2CPP、不装 hook） | 异常与桥接无关 → 查注入/签名（见下） |
| `nebula_no_name.txt` | 跑桥接，但**不写玩家名字** | 异常来自名字写入 |
| 都不放 | 默认（只显示职业名） | — |

**判定顺序**：先放 `nebula_no_bridge.txt`，再放 `nebula_no_name.txt`。

> 历史故障：**速度指数爆炸**（每轮 read→×1.5→write，10 秒 3300 倍）
> 导致玩家飞出地图、Use 键失效、报告键常亮。已修为【基准值×系数】
> 且默认关闭。**跨线程调 IL2CPP** 也已修：现经 GCD 主队列在游戏主线程执行。

**判定顺序**：先放 `nebula_no_bridge.txt`
- ✅ 不崩了 → 问题在游戏桥（IL2CPP 解析 / dump / hook）
- ❌ 还是崩 → 问题在**注入或签名本身**：
  - dylib 是否真的打进了 `Frameworks/` 且路径 `@executable_path/...` 正确
  - 签名是否带 `tools/entitlements.plist`（尤其 `skip-library-validation`）
  - dylib 的架构是否与 App 一致（都是 arm64）

## 常见崩溃原因（已修 / 需注意）

### ① 构造期调用 IL2CPP（曾导致闪退，已修）
`__attribute__((constructor))` 执行时 **Unity 还没初始化 il2cpp**，
此时 `il2cpp_domain_get()` 返回空/野指针，再取 assemblies 直接崩。
**规则：构造期只做 mkdir / 日志 / 起线程；任何 IL2CPP 调用必须等 ready 之后。**
现在轮询线程会先静置 5 秒再探测，且 dump/hook 都在就绪后执行。

### ② 扫描符号表越界（已加校验）
遍历所有镜像（含 dyld 共享缓存）时，部分镜像符号表不完整；
`symoff / stroff / nsyms` 不校验会读到映射区外 → 闪退。
现已按 segment 计算镜像大小并校验，无效则跳过该镜像。

### ③ 替换 `MethodInfo->methodPointer` 撞 PAC
若目标调用点做了指针认证（`autia`/`braa`），替换后会 SIGILL。
判定：放 `nebula_no_hook.txt` 后不崩 → 即为此类。
对策：退回第 3 级（JIT trampoline），或换用其他切入点。

### ④ 插件文件异常
`addons/` 里的 ZIP 若损坏，读取器已做边界处理；
若仍怀疑，先清空 `addons/` 目录再试。

## 看日志定位

日志同时写 syslog 与 `Documents/nebula_log.txt`（需开文件共享）。
电脑用 macOS「控制台」或 `idevicesyslog | grep Nebula-iOS` 查看。

关键行：
- `addons 已加载 N 个插件` → 插件系统正常
- `poll: IL2CPP 已就绪` → 桥接起来了
- `image[...] -> arm64 / arm64e` → ABI 与 PAC 风险
- `poll: hook 安装结果 0` → 帧级拦截成功
- `Update hook 生效中：已 N 帧` → 每帧逻辑在跑
