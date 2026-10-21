// 与游戏通信的 Il2Cpp 桥接桩（沙箱模拟）。
// 真机上换成 Il2CppInterop 生成的真实绑定（Il2Cpp.UnityEngine.* 等）。
namespace Il2Cpp.UnityEngine
{
    public static class Debug
    {
        public static void Log(object o)
        {
            System.Console.WriteLine("[Il2Cpp.UnityEngine.Debug.Log] " + o);
        }
    }
}
