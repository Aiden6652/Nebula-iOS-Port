/*
 * [R61] BepInEx6 IL2CPP 原生 detour 桥接 shim
 * --------------------------------------------------
 * BepInEx6 的 BepInEx.Unity.IL2CPP.Hook.Dobby.DobbyDetour 走「两段式」API：
 *     DobbyPrepare(target, replace, &prepared) -> 分配 trampoline（prepared = 原函数指针）
 *     DobbyCommit(prepared)                    -> 真正把 detour 写入内存
 *     DobbyDestroy(address)                    -> 撤销
 * 但本仓库静态链的是上游 jmpews/dobby，它只有一次性 DobbyHook（prepare+commit 一起完成），
 * 根本没有 DobbyPrepare / DobbyCommit 这两个符号。于是 IL2CPPChainloader.Initialize
 * 在 DobbyDetour.PrepareImpl() 里 P/Invoke "DobbyPrepare" 时抛：
 *     System.EntryPointNotFoundException: DobbyPrepare
 * BepInEx 随即整个退出，游戏直接进原版、NOS 不加载（真机 preloader.log / nebula_host.txt 实锤）。
 *
 * 宿主之前只探测 DobbyHook 是否存在就判定 dobby「已就绪」并跳过 R56 兜底，结果官方 detour
 * 路径在 DobbyPrepare 这步崩。这里用 DobbyHook 包出 DobbyPrepare / DobbyCommit，让官方
 * detour 路径能正常装上，NOS 走正路加载。
 *
 * 语义对齐（BepInEx 调用顺序 = Prepare 紧跟 Commit，不会在中间调用 target）：
 *   - DobbyPrepare：直接 DobbyHook，hook 在此时写好；prepared 填 trampoline（调原函数用）。
 *   - DobbyCommit ：hook 已在 Prepare 写完，这里为空操作，返回 0（成功）。
 *   - DobbyDestroy：上游 dobby 已有，不再重复定义。
 *
 * 注：宿主本身用 dlsym 取 DobbyHook，编译期不 include dobby.h，故这里前向声明即可，
 * 不依赖头文件搜索路径。
 */
#ifdef __cplusplus
extern "C" {
#endif

/* 上游 jmpews/dobby 的真实签名（dobby_dummy_func_t == void*）。 */
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
