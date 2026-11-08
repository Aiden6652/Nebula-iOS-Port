// AUI.ServerScroll — [R82] 用 BepInEx/Harmony 劫持 ServerDropdown.FillServerOptions 搭滑动
//
// 为什么推翻 R60 版：
//   R60 版 patch 的目标是 ServerDropdown.OpenServerDropdown，还想找 UGUI 的
//   ScrollRect —— 这两样在 AU iOS 17.3.1o 的 interop 里【都不存在】。AU 的 UI 是
//   2D SpriteRenderer + Collider2D + PassiveButton + ObjectPoolBehavior，不是
//   UGUI Canvas。所以 R60 版即使编进 DLL，Harmony 也会因目标方法找不到而静默失效，
//   等于交了个空包（这正是用户说的「滑动没做」）。
//
// R82 做法（用户思路：BepInEx 能劫持 AU 画面，直接用它自己搭）：
//   1) Harmony patch 真实存在的 ServerDropdown.FillServerOptions（Postfix）——
//      每次下拉打开、ButtonPool 生成完全部服务器按钮之后必调。
//   2) 把 ButtonPool.activeChildren 的按钮 reparent 到一个新建的内容容器
//      AUI_ScrollContent（保持世界坐标，视觉零变化）。
//   3) 在 ServerDropdown 上 AddComponent<Scroller>() —— 这是 AU【原生】的 2D
//      滚动组件（聊天窗口 ChatController.scroller 用的就是它），继承
//      PassiveUiElement，自带每帧 Update / ReceiveClickDrag / 惯性 / 滚轮，
//      不需要 ClassInjector 注册新 IL2CPP 类型（那个在 iOS 上会挂）。
//   4) 配 Inner=内容容器、ContentYBounds=可滚范围、allowY=true，再给
//      Colliders/ClickMask 一个覆盖可见区的 BoxCollider2D 做 hit-test。
//   5) 顺带把下拉背景拉长到能看见更多项（用户备选方案「或者把列表拉长」）。
//
// 安全：全程 try/catch，任何一步失败只写日志，绝不影响 NOS / 游戏主流程。
//   插件本身不注入任何原生类型，纯托管 + 游戏已有的 Scroller 组件。

using System;
using System.Collections.Generic;
using BepInEx;
using BepInEx.Unity.IL2CPP;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;
using Il2CppInterop.Runtime.InteropTypes.Arrays;

namespace AUI
{
    [BepInPlugin("com.aiden.aui.serverscroll", "AUI Server Scroll", "2.0.0")]
    public class ServerScrollPlugin : BasePlugin
    {
        internal static ManualLogSource L;

        public override void Load()
        {
            L = Log;
            try
            {
                new Harmony("com.aiden.aui.serverscroll").PatchAll();
                L.LogInfo("[AUI.ServerScroll] R82 已加载：hook ServerDropdown.FillServerOptions -> AU 原生 Scroller 竖向滑动");
            }
            catch (Exception e)
            {
                // 插件失败绝不能拖垮 BepInEx / NOS
                L.LogError("[AUI.ServerScroll] Load 失败（忽略，不影响游戏）: " + e);
            }
        }

        [HarmonyPatch(typeof(ServerDropdown), "FillServerOptions")]
        public static class PatchFillServerOptions
        {
            static void Postfix(ServerDropdown __instance)
            {
                try { Apply(__instance); }
                catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] Apply 异常（忽略）: " + e); }
            }
        }

        private const string ContentName = "AUI_ScrollContent";

        static void Apply(ServerDropdown d)
        {
            if (d == null) return;
            var pool = d.ButtonPool;
            if (pool == null) { L?.LogWarning("[AUI.ServerScroll] ButtonPool 为空，跳过"); return; }
            var children = pool.activeChildren;   // Il2CppSystem.Collections.Generic.List<PoolableBehavior>
            int n = children == null ? 0 : children.Count;
            L?.LogInfo("[AUI.ServerScroll] FillServerOptions: 服务器按钮 " + n + " 个");
            if (n <= 3) return;   // 一屏放得下，不折腾

            // ---- 1) 找/建内容容器；先把上次挂进来的按钮还回 d.transform（幂等）----
            Transform content = null;
            for (int i = 0; i < d.transform.childCount; i++)
            {
                var c = d.transform.GetChild(i);
                if (c != null && c.name == ContentName) { content = c; break; }
            }
            if (content == null)
            {
                var go = new GameObject(ContentName);
                go.transform.SetParent(d.transform, false);
                go.transform.localPosition = Vector3.zero;
                go.transform.localScale = Vector3.one;
                content = go.transform;
            }
            else
            {
                var old = new List<Transform>();
                for (int i = 0; i < content.childCount; i++) old.Add(content.GetChild(i));
                foreach (var t in old) if (t != null) t.SetParent(d.transform, true);
            }

            // ---- 2) 把所有服务器按钮 reparent 进内容容器（保持世界坐标）----
            float cellH = 0.7f;
            try { if (ServerDropdown.YOFFSET_BUTTON > 0.01f) cellH = ServerDropdown.YOFFSET_BUTTON; } catch { }
            for (int i = 0; i < n; i++)
            {
                var b = children[i];
                if (b == null) continue;
                var t = b.transform;
                if (t == null) continue;
                t.SetParent(content, true);   // worldPositionStays：视觉零变化
            }
            L?.LogInfo("[AUI.ServerScroll] 内容容器就位，按钮 " + content.childCount + " 个，间距 " + cellH.ToString("F3"));

            // ---- 3) 取/挂 AU 原生 Scroller ----
            Scroller sc = null;
            try { sc = d.GetComponent<Scroller>(); } catch { }
            if (sc == null)
            {
                try { sc = d.gameObject.AddComponent<Scroller>(); }
                catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] AddComponent<Scroller> 失败: " + e); }
            }
            if (sc == null) { L?.LogWarning("[AUI.ServerScroll] 拿不到 Scroller，仅靠背景拉长兜底"); return; }

            // ---- 4) 可见区高度 & 可滚范围 ----
            float visibleH = 3f * cellH;
            try { if (ServerDropdown.INITIAL_HEIGHT > 0.1f) visibleH = ServerDropdown.INITIAL_HEIGHT; } catch { }
            float totalH = Mathf.Max(visibleH, n * cellH + cellH);
            float maxScroll = Mathf.Max(0f, totalH - visibleH);

            // hitbox：覆盖可见区的 BoxCollider2D，供 PassiveUiElement 每帧 hit-test
            try
            {
                BoxCollider2D hit = null;
                try { hit = d.GetComponent<BoxCollider2D>(); } catch { }
                if (hit == null) hit = d.gameObject.AddComponent<BoxCollider2D>();
                float w = 10f;
                try { if (d.background != null) w = Mathf.Max(2f, d.background.bounds.size.x); } catch { }
                hit.size = new Vector2(w, visibleH);
                hit.center = new Vector2(0f, -(visibleH - cellH) * 0.5f);
                var arr = new Il2CppReferenceArray<Collider2D>(1);
                arr[0] = hit;
                sc.Colliders = arr;
                sc.ClickMask = hit;
            }
            catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] hitbox 配置失败（忽略）: " + e); }

            // ---- 5) 配置滚动（对称 bounds：不管 AU 内部方向如何都能拖动）----
            try
            {
                sc.Inner = content;
                sc.allowY = true;
                sc.allowX = false;
                sc.showY = false;              // 不显示滚动条（没有 ScrollbarY 对象，避免空引用）
                sc.showX = false;
                sc.MouseMustBeOverToScroll = false;
                sc.DragScrollSpeed = 1f;
                sc.ScrollWheelSpeed = 1f;
                sc.ContentYBounds = new FloatRange(-maxScroll, maxScroll);
                sc.active = true;
                sc.ScrollToTop();
                L?.LogInfo("[AUI.ServerScroll] Scroller 已配置：maxScroll=" + maxScroll.ToString("F2")
                           + "，Inner=" + content.childCount + " 项，竖向滑动开启");
            }
            catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] Scroller 配置失败（忽略）: " + e); }

            // ---- 6) 把背景拉长（用户备选方案），让更多服务器同时可见 ----
            try
            {
                if (d.background != null)
                {
                    var s = d.background.size;
                    if (s.y < visibleH) d.background.size = new Vector2(s.x, visibleH);
                }
            }
            catch (Exception e) { L?.LogWarning("[AUI.ServerScroll] 背景拉长失败（忽略）: " + e); }
        }
    }
}
