/*
 * [R61] BepInEx6 IL2CPP 原生 detour 桥接 shim（legacy-mono 构建同源）
 * --------------------------------------------------
 * 见 ../mono_host/nebula_dobby_shim.c 说明。legacy-mono 走 BepInEx5(Mono) 后端，
 * 本身不调 Dobby；这里仍把 shim 链进来，仅为让同一个 dobby 静态库在两条构建里
 * 都暴露 DobbyPrepare/DobbyCommit，防止该 dylib 被误用于 IL2CPP 游戏时复现同类崩溃。
 */
#ifdef __cplusplus
extern "C" {
#endif

int DobbyHook(void *address, void *replace_func, void **origin_func);

#ifdef __cplusplus
}
#endif

__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    return DobbyHook(target, replace, prepared);
}

__attribute__((visibility("default")))
int DobbyCommit(void *prepared) {
    (void)prepared;
    return 0;
}
