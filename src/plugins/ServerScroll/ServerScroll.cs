// AUI.ServerScroll — [R60] 给 Among Us 服务器下拉列表加滑动 / 拉长列表
//
// 背景：原版 ServerDropdown 一屏只放得下 3 个服务器，私服多时后面的按钮全飞出屏幕。
//   iOS 版 17.3.1o 的 interop 里 ServerDropdown 已自带 RecyclableScrollRect 系列滚动
//   基础设施（见 Assembly-CSharp.dll 的 MaxCount / CellSize / spacingY），本补丁不改
//   游戏逻辑，只在下拉打开的瞬间：把 viewport 拉长、把 content 高度按实际子项数量
//   撑开、强制 vertical 滚动 —— 等价于用户要的「改成支持滑动的 ScrollRect / 把列表拉长」。
//
// 安全：全程 try/catch，任何一步失败只写日志、绝不影响 NOS / 游戏主流程。
//   Harmony patch 目标为 ServerDropdown.OpenServerDropdown（public void）。

using System;
using BepInEx;
using BepInEx.Unity.IL2CPP;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

namespace AUI
{
    [BepInPlugin("com.aiden.aui.serverscroll", "AUI Server Scroll", "1.0.0")]
    public class ServerScrollPlugin : BasePlugin
    {
        internal static ManualLogSource L;

        public override void Load()
        {
            L = Log;
            try
            {
                new Harmony("com.aiden.aui.serverscroll").PatchAll();
                L.LogInfo("[AUI.ServerScroll] 已加载：服务器列表滑动/拉长补丁");
            }
            catch (Exception e)
            {
                // 插件失败绝不能拖垮 BepInEx / NOS
                L.LogError("[AUI.ServerScroll] Load 失败（忽略，不影响游戏）: " + e);
            }
        }

        [HarmonyPatch(typeof(ServerDropdown), "OpenServerDropdown")]
        public static class PatchOpenServerDropdown
        {
            public static void Postfix(ServerDropdown __instance)
            {
                try { Apply(__instance); }
                catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] Apply 异常（忽略）: " + e); }
            }
        }

        private static void Apply(ServerDropdown d)
        {
            if (d == null) return;

            // 1) 找 ScrollRect（RecyclableScrollRect 派生自它），强制竖向滚动
            ScrollRect scroll = d.GetComponentInChildren<ScrollRect>();
            if (scroll != null)
            {
                scroll.vertical = true;
                scroll.horizontal = false;

                // 1a) 拉长 viewport：原来只够 3 项，撑到至少 600px
                RectTransform vp = scroll.viewport as RectTransform;
                if (vp != null && vp.rect.height < 600f)
                {
                    Vector2 s = vp.sizeDelta;
                    vp.sizeDelta = new Vector2(s.x, Mathf.Max(s.y, 600f));
                }

                // 1b) content 按实际子项数量撑开，否则 ScrollRect 认为没内容可滚
                RectTransform content = scroll.content;
                if (content != null)
                {
                    int n = content.childCount;
                    if (n > 3)
                    {
                        float cell = 60f;
                        RectTransform c0 = content.GetChild(0) as RectTransform;
                        if (c0 != null && c0.rect.height > 1f) cell = c0.rect.height;
                        float want = cell * n + 40f;
                        if (content.sizeDelta.y < want)
                            content.sizeDelta = new Vector2(content.sizeDelta.x, want);
                    }
                }
                L?.LogInfo("[AUI.ServerScroll] ScrollRect 已启用竖向滚动并拉长");
            }
            else
            {
                L?.LogWarning("[AUI.ServerScroll] 未找到 ScrollRect，仅拉长根容器");
            }

            // 2) 拉长下拉根容器（用户备选方案「或者把列表拉长」）
            RectTransform root = d.transform as RectTransform;
            if (root != null && root.sizeDelta.y < 700f)
                root.sizeDelta = new Vector2(root.sizeDelta.x, 700f);
        }
    }
}
