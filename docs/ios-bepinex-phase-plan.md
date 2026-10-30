# iOS 移植路线：BepInEx/Il2CppInterop 接入计划（B 路线）

> 最后更新：2026-10-30
> 决策：**选 B（用我们已打通的嵌入 Mono 作为 Il2CppInterop 的 Mono domain），放弃 C（native doorstop）**。
> 理由：iOS 非越狱下没有 `GameAssembly.dll` 可换、没有 DLL 代理加载机制，C 的 doorstop 整套用不了且会推翻已打通的 Mono JIT 架构。B 是直接复用桌面同款 BepInEx.IL2CPP 托管层，只把"它自带的 Mono"换成"我们的 Mono"。

---

## 已确认的事实（地基）

| 项 | 结论 | 来源 |
|---|---|---|
| 嵌入 Mono JIT | 可用（StikDebug 调试态 + `mprotect` RWX 翻转，mem 日志 prot=3↔5 实锤） | mem 日志 |
| CoreLib 启动 | `getenv` 那道坎已过，原生 shim `libSystem.*.dylib` 部署后正常 | host 日志 |
| 原生游戏 | 已到主菜单（`MainMenuManager`），UnityFramework 在设备端正常加载运行 | host 日志 |
| il2cpp 位置 | 在 `UnityFramework.framework/UnityFramework`（arm64 dylib，**cryptid=0 未加密**） | 沙箱解包 |
| il2cpp 导出 | `il2cpp_runtime_invoke` / `object_new` / `domain_get` / `class_from_name` / `resolve_icall` / `string_new` / `thread_attach` / `method_get_name` 等均在导出表 | 字节扫描 |
| 元数据 | `global-metadata.dat` 磁盘**加密**（sanity `0xfab11baf`、version 31 / Unity 2022.3.44f1），运行时才解密 | 沙箱解包 |
| 版本一致性 | 17.3.1 与 19.0 的 `UnityFramework` + `global-metadata.dat` **md5 完全一致** → 底层零差异 | 字节比对 |
| BepInEx 现状 | `BepInEx.Unity.IL2CPP` 已从 blob 加载，`Doorstop.Entrypoint.Start()` 抛异常导致模组层未初始化 | host 日志 |

**核心架构图：**
```
我们的宿主 dylib（嵌入 Mono，已验证 JIT）
   ├─ CoreLib 启动 ✓
   ├─ 加载 BepInEx.Unity.IL2CPP（托管层，桌面同款）✓ 已加载
   ├─ 同一进程内 dlsym UnityFramework 的 il2cpp_* 运行时 API  ← Phase 2
   ├─ 喂解密后的 global-metadata.dat 给 Il2CppInterop      ← Phase 1
   └─ 用 mprotect 翻转实现 Il2CppInterop detour（改原生代码）← Phase 3
         └─ 加载 Nebula.dll + HarmonyX 补丁生效             ← Phase 4
```

---

## Phase 0 — 拿真异常（进行中）

**目标**：确认 `Start()` 究竟缺什么，避免盲改。

- 用 `54fa709`（已修前向声明编译错误，CI 已触发）出包。
- 部署方式不变：6 个 dylib 全拖进 `AmongUs.app/` + 重签。
- 回传 `nebula_host.log`，重点看：
  - `[host]   异常内容: ...`
  - `[host]   异常类型: ...`
- 这一行会告诉我们 `Start()` 缺的是"元数据没喂"还是"il2cpp 运行时没接"。

**判定分支**：
- 报 `FileNotFoundException` / 找不到 `GameAssembly` / 找不到 metadata → 指向 Phase 1。
- 报 il2cpp 初始化 / `il2cpp_*` 相关 → 指向 Phase 2。
- 报 Unity 程序集 / `UnityEngine.*` 缺失 → 指向 Phase 1 的代理程序集生成。

---

## Phase 1 — 元数据供给（Metadata Provisioning）

**目标**：产出 Il2CppInterop 能吃的、解密的 `global-metadata.dat` + 代理托管程序集。

### 1.1 解密 metadata（二选一）

- **路线 A（推荐，运行时抓内存）**：
  - 宿主在初始化时从 `UnityFramework` `dlsym` 到 il2cpp 元数据加载函数（如 `il2cpp::vm::MetadataLoader::LoadMetadataFile` / `il2cpp_metadata_get_method_pointer` 相关）。
  - hook 该函数，拦截解密后的 metadata 内存块，落盘到 app 可写目录。
  - 优势：不用逆向加密算法，必然拿到游戏自己解出来的明文。
- **路线 B（静态逆向）**：
  - `UnityFramework` 已解密（cryptid=0），在二进制内定位 metadata 解密例程与 key/种子，复现解密。
  - sanity `0xfab11baf` 是 Unity 2022.3 的 metadata 加密特征（整文件块加密 + 内嵌 key），逆向工作量较大，作为 A 的兜底。

### 1.2 生成代理程序集

- 工具链：**Cpp2IL**（吃 arm64 Mach-O，支持 Unity 2022.3 / metadata v31）→ **Il2CppInterop** 生成代理托管程序集。
- 输入替换：桌面 Il2CppInterop 默认吃 `GameAssembly.dll`；iOS 上改为喂 `UnityFramework`（解密后的 arm64 Mach-O，Cpp2IL 支持 Mach-O 格式）。
- 产物放到 `AmongUs.app/BepInEx/interop/`（与桌面布局一致）。

### 1.3 喂给 BepInEx

- 设置 `IL2CPP_INTEROP_PATH` / BepInEx 的 interop 目录指向上面生成的代理程序集。
- 确保 BepInEx 启动时优先从本地 interop 加载，而非尝试自己找 `GameAssembly`。

---

## Phase 2 — il2cpp 运行时接线（Runtime Wiring）

**目标**：让 Il2CppInterop 能调用 `UnityFramework` 里的 il2cpp 原生运行时。

### 2.1 符号解析

- 宿主用 `dlopen("UnityFramework.framework/UnityFramework", ...)` + `dlsym` 取到全套 `il2cpp_*` 函数指针。
- 需要完整枚举 Il2CppInterop 依赖的 il2cpp API 集合（不只是已确认的 8 个，需列全：域/对象/类/方法/字符串/GC/线程/icall/程序集/图像等）。

### 2.2 适配层

- Il2CppInterop 通过一组"il2cpp 原生 API 委托"工作，桌面版绑定到 `GameAssembly` 的导出。
- 写适配器：把这些委托重定向到我们在 Phase 2.1 `dlsym` 到的 `UnityFramework` 指针。
- 关键难点：Il2CppInterop 可能假设自己是进程内唯一的 il2cpp 运行时，而我们是要"挂在游戏已初始化的 il2cpp 旁边"——需要确认它能 attach 到已运行的 il2cpp 域（用 `il2cpp_thread_attach` / `il2cpp_domain_get`）。

### 2.3 验证

- 单独写个最小探针：从宿主调 `il2cpp_domain_get` / `il2cpp_object_new` 能否成功创建 il2cpp 对象 → 证明接线通。

---

## Phase 3 — Detour / Hook 能力（改原生代码）

**目标**：让 Il2CppInterop `ClassInjector.Detour` 与 HarmonyX 能重定向原生 il2cpp 方法。

- **底层原语已具备**：mem 日志实锤的 `mprotect` RWX 翻转（prot=3↔5，ret=0）正是 detour 要的"运行时改写原生代码"能力。
- 实现 detour：
  1. `mprotect` 目标页为 RWX（复用我们已验证的翻转路径）；
  2. 写入跳转/trampoline 到我们的托管桥接；
  3. `mprotect` 还原为 R-X。
- 让 Il2CppInterop 的 detour 后端走我们这套 mprotect 实现，而非它默认依赖的 `VirtualProtect`（Windows）/ `mprotect`（Unix，逻辑相同，但需确认 iOS 上权限与 CS_DEBUGGED 调试态配合）。

---

## Phase 4 — 加载 Nebula + 验证

**目标**：模组真正改到游戏行为。

- BepInEx 插件系统在 Phase 1/2/3 就绪后加载 `Nebula.dll`。
- HarmonyX 补丁通过 Phase 3 的 detour 应用到原生 il2cpp 方法。
- **验证标准**：找一个已知的托管补丁（如改某个 UI 文案或游戏数值），确认它在真机上确实改变了原生游戏表现 → 整条链（Mono 嵌入 → BepInEx → Il2CppInterop → detour → Nebula）闭环。

---

## 待确认的关键未知（决定 Phase 1/2 怎么写）

1. **metadata 加密算法**：Unity 2022.3.44 的具体方案（整文件块加密 + 内嵌 key），路线 A 可绕过，但需确认 hook 点存在且可拦截。
2. **il2cpp API 完整集合**：Il2CppInterop 实际依赖哪些 `il2cpp_*` 符号，`UnityFramework` 是否全部导出（少量可能走内部链接，需另找）。
3. **域 attach 兼容性**：我们的嵌入 Mono 新建的 AppDomain 能否与游戏已初始化的 il2cpp 运行时共存、Il2CppInterop 能否 attach。
4. **版本号异常**：IPA 文件名 `2026.17.3.1` 但 plist `CFBundleShortVersionString=2026.4.7`，虽底层一致不影响移植，但需你确认用的到底是哪个包。

---

## 与先前崩溃修复的对应关系

| 轮次 | 崩点 | 状态 |
|---|---|---|
| ≤19 | W^X 写崩（str w9 SIGSEGV） | 已修（mprotect 引用计数） |
| 20 | DllNotFoundException（缺 libSystem.* shim） | 已修（落盘 + dllmap + 中和 DISABLE_DLLMAP） |
| 21 | `conflicting types mono_dllmap_insert` 编译错 | 已修（删本地 extern） |
| 22 | shim 漏部署 | 加固（多位置搜索 + 双份落盘） |
| 23 | BepInEx `Start()` 抛异常（模组层未初始化） | 诊断中（异常内容打印已修编译，CI 出包） |
| 24+ | BepInEx/Il2CppInterop 真正接入 | 见上 Phase 1–4 |
