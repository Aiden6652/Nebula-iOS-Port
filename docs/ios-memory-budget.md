# iOS 内存预算：全量 NOS 防 OOM

> 目标：把**全量 NOS**（星云舰 + 28 个 Addons）塞进 iOS，且在**没有 `increased-memory-limit` 权限**的设备上不被 jetsam 杀掉。
> 本文所有体积数据来自对 PC 改装包 `星云3413.zip` 的实测清点。

## 1. 实测：全量 NOS 到底多重

| 组成 | 体积 | 备注 |
|---|---|---|
| `BepInEx/interop/` | **50.2 MB** | Il2CppInterop 生成的 unhollowed 包装层 |
| `BepInEx/nebula/Nebula.dll` | **32.4 MB** | 星云舰本体 |
| `NebulaLibs/` | **17.1 MB** | 含 Roslyn（`Microsoft.CodeAnalysis.CSharp.dll` 6.6 MB）+ `Interstellar.dll` 6.4 MB |
| `Addons/` | **110 MB / 28 个** | 全是 zip：语言包、皮肤、贴图、脚本 |
| `BepInEx/core/` | 6.2 MB | BepInEx 框架 |
| `BepInEx/plugins/` | 0.2 MB | |
| 游戏本体（GameAssembly/UnityPlayer/资源）| ~1.2 GB | **不属于我们**，但和我们一起抢内存 |

最大的几个 dll：`GameAssembly.dll` 45.6 MB → `Nebula.dll` 32.4 MB → `UnityPlayer.dll` 21.9 MB →
`Assembly-CSharp.dll`(interop) 7.1 MB → `Microsoft.CodeAnalysis.CSharp.dll` 6.6 MB → `Interstellar.dll` 6.4 MB。

**关键换算**：程序集加载成托管元数据后，常驻内存通常是文件大小的 **3～5 倍**。
所以 50 MB 的 interop 实际可能占 150～250 MB，32 MB 的 Nebula.dll 占 100～160 MB。

## 2. iOS 的红线

- 没 `com.apple.developer.kernel.increased-memory-limit` 时，进程被 jetsam 杀掉的阈值随设备 RAM 变化，大致是**几百 MB 到 1GB 出头**的量级。
- AU 本体（IL2CPP + Unity，几十 MB 的 `GameAssembly` 加上资源）本身就要吃掉相当一部分。
- **结论：给模组层的预算应压在 250～350 MB**，并且必须能降级。

分发场景尤其如此——你自己能开大内存权限，不代表别人能。所以**默认配置按"最差的设备"来定**。

## 3. 预算分配（目标值，待真机校准）

| 项目 | 预算 | 说明 |
|---|---|---|
| Mono 运行时 + BCL | ~60 MB | Godot 的 minimal 配置已砍掉 remoting/COM/appdomains/security 等，够瘦 |
| interop 包装层（按需加载）| ~120 MB | 最大头，靠"按需"而不是"全量预载"压下来 |
| Nebula.dll + Interstellar | ~100 MB | |
| Addons（默认只开必要项）| ~50 MB | 28 个全开是 110 MB，**不能全开** |
| JIT/解释器代码 + GC 堆 | ~50 MB | |
| **合计目标** | **~350 MB** | 软顶设 320 MB，超了强制 GC |

## 4. 六条硬规则

1. **绝不预加载。** Mono 默认是按需加载程序集的——别写循环去 `mono_assembly_open` 全部 interop dll。让引用驱动加载。
2. **Addons 默认只开必要项。** 语言包（SCLangPack / Plan17Language）必开，皮肤/贴图/剧本类按需开。28 个全开 = 110 MB 起步，必炸。
3. **Roslyn 只在"真的要编译 addon 脚本"时加载。** 更好的做法是**打包期在 PC 上把 addon 脚本预编译成 dll**，运行时根本不碰 Roslyn——省掉 6.6 MB 的文件和上百 MB 的编译期工作集峰值。
4. **GC 调小 + RSS 软顶。** nursery 调小（默认可能偏大），并起一个看门狗线程采样 RSS，超软顶就 `mono_gc_collect()`。见 `nebula_host.cfg`。
5. **不分发 PC 专用的东西。** `dotnet/`（CoreCLR，~40 MB）、`GameAssembly.dll`、`UnityPlayer.dll`、`EOSSDK` 一律不带——iOS 上要么用不上，要么本来就是游戏自带的。
6. **JIT 不是默认项。** 没有 `dynamic-codesigning` 的设备上 JIT 直接崩。默认 `runtime.mode = interp`（解释器，不需要可执行内存、不需要权限，处处能跑），能开 JIT 的人自己改配置切 `jit` 换性能。

## 5. 降级阶梯（内存不够时依次砍）

1. 关 Addons（保留语言包）→ 省 ~60 MB
2. `runtime.mode` 从 `jit` 退回 `interp` → 省 JIT 代码缓存
3. 关 addon 脚本编译（不加载 Roslyn）→ 省峰值 ~100 MB
4. 砍 interop：只保留 Nebula 真正用到的程序集 → 省 ~80 MB
5. 还不行就只保留星云舰核心，Addons 全关

## 6. 待真机校准

上面的预算是**估算**，不是实测。等 iOS 版跑起来后要在真机上量三件事再回填本表：

- Mono 初始化完成后的基线 RSS
- BepInEx + Nebula 加载完的 RSS
- 逐个开启 Addon 的增量 RSS

校准完把软顶 `gc.soft_limit_mb` 调到"实测峰值 + 20% 余量"。
