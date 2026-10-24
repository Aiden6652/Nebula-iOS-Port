# iOS 内存预算：全量 NOS 防 OOM

> **硬约束：28 个 Addons 必须全部加载** —— 职业定义就写在 addon 的脚本里，漏一个就玩不了。
> 所以省内存不能靠"少加载 addon"，只能从别处省。
> 本文所有体积数据来自对 PC 改装包 `星云3413.zip` 的实测清点。

## 1. 单一版本（编译器 / JIT）

> 解释器已废除，原 Standard / Pro 合并为一份。实测两种旧模式都要在
> `mono_trampolines_init` 生成 trampoline，解释器并未省下 JIT 能力，反而更慢、
> 还多一套构建分支，故只出一个包（见 `nebula_host.standard.cfg`）。

| | **统一编译器（JIT）版** |
|---|---|
| 运行模式 | `jit` 强制 JIT |
| 需要 `dynamic-codesigning` | **是**（没有会直接崩）|
| 需要 `increased-memory-limit` | **建议**（软顶放到 512 MB，防 jetsam）|
| GC nursery | 32 MB |
| RSS 软顶 / 硬顶 | 512 MB / 500 MB |
| 贴图长边上限 | 1024 px |
| 贴图 LRU 缓存 | 128 MB |
| **目标帧率** | **不限制（0）** |
| 掉帧自适应降画质 | 开 |
| Addons | **全部加载** |
| Roslyn | 关（打包期预编译）|

选择方式：默认即本配置（JIT 版）；可用 `NEBULA_HOST_CFG=<路径>` 直接指定配置文件。

**为什么统一走 JIT**：iOS 上 mono 初始化必然生成 trampoline，两种模式都要 JIT 能力；
既如此，直接用编译器版（更快、构建更简单），JIT 权限由 StikDebug / 全能签 entitlements 提供。

**为什么 Standard 锁 60 帧**：解释器比 JIT 慢，同样的算力摊到每一秒里，更容易出现"某几帧做不完"，表现就是卡顿。**锁帧是把"每秒的算力"换成"每帧的预算"**——把上限压住，每帧就有确定的时间窗，宁可稳定 60，也不要忽高忽低。Pro 走 JIT 够快，不需要这个妥协，直接放开让设备跑满。

两版都保留 `render.adaptive = 1`：真掉帧时先降贴图分辨率、再压 LRU 缓存，而不是硬扛着掉帧。（自适应逻辑在托管侧实现，宿主只负责把配置透传成 `NEBULA_TARGET_FPS` / `NEBULA_ADAPTIVE`。）

## 2. 实测：全量 NOS 多重

| 组成 | 体积 | 说明 |
|---|---|---|
| `BepInEx/interop/` | 50.2 MB | Il2CppInterop 生成的 unhollowed 包装层 |
| `BepInEx/nebula/Nebula.dll` | 32.4 MB | 星云舰本体 |
| `NebulaLibs/` | 17.1 MB | 含 Roslyn 6.6 MB + `Interstellar.dll` 6.4 MB |
| `Addons/` | 110 MB（压缩）／**123 MB（解压）** | 28 个 |
| `BepInEx/core/` | 6.2 MB | 框架 |

### Addons 内部构成（这是关键）

| 类型 | 体积 | 数量 | 性质 |
|---|---|---|---|
| `.png` | **97.82 MB** | 2423 | **纯外观**（皮肤/帽子/印章/服装）|
| `.zip`（嵌套资源包）| 10.06 MB | 1 | 资源 |
| `.mp3` | 9.51 MB | 3 | 音频 |
| `.cs` | **3.55 MB** | 266 | **职业逻辑 — 必须加载** |
| `.dat` | 0.67 MB | 51 | |
| `.dll` | **0.64 MB** | 7 | **必须加载** |
| `.json` | 0.53 MB | 14 | |
| `.bundle` | 0.49 MB | 2 | |

**结论：职业逻辑全加起来只有 4.2 MB（`.cs` + `.dll`），外观资源却占了 117 MB（95%+）。**

也就是说 ——**"addons 全部加载"和"防 OOM"根本不冲突**。全加载是必须的，而且很便宜；真正会撑爆内存的只有贴图。

## 3. 为什么贴图才是杀手

97.82 MB 是 **png 压缩后**的体积。解码成 RGBA 常驻内存会放大好几倍：一张 2048×2048 的 RGBA 就是 16 MB，2423 张全解码是灾难。

所以贴图三条铁律：

1. **按需解码** —— 只有真正要显示的才解码，不做启动预载
2. **降分辨率** —— 长边压到 512 px（Standard）／1024 px（Pro）
3. **LRU 上限** —— 缓存 64 MB（Standard）／128 MB（Pro），超了淘汰最久未用的

## 4. 预算表（Standard，目标值）

| 项目 | 预算 |
|---|---|
| Mono 运行时 + BCL | ~60 MB（Godot minimal 已砍掉 remoting/COM/appdomains/security）|
| interop 包装层（引用驱动，不预载）| ~120 MB |
| `Nebula.dll` + `Interstellar.dll` | ~100 MB |
| Addons 职业逻辑（**全量**）| ~15 MB |
| 贴图 LRU 常驻 | 64 MB（硬上限）|
| 解释器 + GC 堆 | ~50 MB |
| **合计** | **~350 MB**（软顶设 320 MB）|

## 5. 硬规则

1. **Addons 全加载**（职业）——不可协商，别为了省内存去砍。
2. **贴图按需解码 + 降分辨率 + LRU 上限** —— 这是唯一的大头杠杆。
3. **音频按需**：9.5 MB 的 `.mp3` 不预载。
4. **嵌套 zip 按需解包**：`Plana's CustomAchievementsLoad` 里那个 10 MB 的嵌套包不要启动就展开。
5. **Roslyn 关**：addon 脚本在打包期于 PC 上预编译成 dll，运行时不加载 Roslyn（省上百 MB 编译峰值）。
6. **interop 不预载**：让程序集引用驱动加载，别循环 `mono_assembly_open` 全部包装层。
7. **不带 PC 专用件**：`dotnet/`（CoreCLR ~40 MB）、`GameAssembly.dll`、`UnityPlayer.dll` 一律剔除。
8. **RSS 看门狗**：采样 RSS，超软顶就 `mono_gc_collect()`。

## 6. 降级阶梯（内存仍不够时，按顺序砍）

1. 贴图：上限 512→256 px，LRU 64→32 MB → 省 ~35 MB
2. 音频彻底不加载 → 省 ~10 MB
3. interop 只保留实际被引用到的程序集 → 省 ~80 MB
4. 以上都做了还不行 → 该设备确实跑不动全量 NOS，只能提示用户改用精简 addon 组合

注意：**第 4 步是最后手段**。职业在 addon 里，砍 addon 等于砍玩法。

## 7. 待真机校准

上面全是**估算**，不是实测。iOS 版跑起来后要量三个数回填：

- Mono 初始化完成后的基线 RSS
- BepInEx + Nebula + 全部 addon 职业逻辑加载完的 RSS
- 贴图 LRU 打满时的峰值 RSS

校准完把 `gc.soft_limit_mb` 设成"实测峰值 + 20% 余量"。
