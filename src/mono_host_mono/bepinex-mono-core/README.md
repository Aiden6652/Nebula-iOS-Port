# BepInEx5 Unity Mono core（vendored，构建期嵌进 dylib）

本目录放 **BepInEx 5 的 Unity Mono 版** 托管 dll（不是 main 分支用的 BepInEx 6 / IL2CPP 版）。

## 必需文件
至少包含以下 dll（从 BepInEx 5.x 的 `BepInEx/core/` 拷过来）：

- `BepInEx.Preloader.dll` ← 宿主入口（`boot_bepinex` 调它的 `Preloader.Main(string[])`）
- `BepInEx.Core.dll` ← 宿主 `blob_find("BepInEx.Core")` 用到
- `BepInEx.Unity.Mono.dll` ← Unity Mono 平台的 chainloader / 路径逻辑
- `MonoMod.*.dll`、`0Harmony.dll`、`HarmonyX.dll`、`Mono.Cecil.*.dll` 等依赖

**不要放 `dobby.dll`**（原生库，构建脚本会自动排除，dobby 已静态链进主 dylib）。

## 版本要求
- 必须是 **Unity Mono 后端** 对应的 BepInEx 5（桌面老 AU 那个时代的构建）。
- 与 NOS / 老模组的 Mono 版构建同源，版本需匹配，否则插件加载期 API 错配。

## 怎么拿
1. 从 NOS 的老版本 Mono 构建包里取（用户已确认 NOS 有老 Mono 版）；或
2. 从 BepInEx 官方 releases 下 `BepInEx_5.x.x_UnityMono.zip`，解压后取 `BepInEx/core/`。

放好后 `bash src/mono_host_mono/build_ios_mono.sh <Mono前缀>` 即可（CI 也会自动取）。
