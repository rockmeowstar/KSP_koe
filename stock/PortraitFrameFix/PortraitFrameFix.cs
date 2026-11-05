using System;
using System.Collections;
using System.Reflection;
using HarmonyLib;
using KSP.UI.Screens.Flight;
using KSP.UI.Util;
using UnityEngine;
using UnityEngine.UI;

[assembly: AssemblyVersion("1.0.0.0")]

namespace KerbalOverEngineering.Portraits
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class PortraitFrameFix : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.PortraitFrameFix";
        static readonly FieldInfo CabinPortrait = AccessTools.Field(typeof(Kerbal), "portrait");
        static readonly FieldInfo EvaPortrait = AccessTools.Field(typeof(KerbalEVA), "portrait");
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.PortraitFullRate") && !KerbalOverEngineering.Options.Settings.Enabled("Stock.PortraitHiddenCull")) { Destroy(this); return; }
            try
            {
                var harmony = new Harmony(Id);
                if (CabinPortrait == null || EvaPortrait == null) throw new MissingFieldException("portrait");
                foreach (Type type in new[] { typeof(Kerbal), typeof(KerbalEVA) })
                {
                    MethodInfo method = AccessTools.DeclaredMethod(type, "kerbalAvatarUpdateCycle");
                    if (method == null || method.ReturnType != typeof(IEnumerator)) throw new MissingMethodException(type.Name, "kerbalAvatarUpdateCycle");
                    harmony.Patch(method, postfix: new HarmonyMethod(typeof(PortraitFrameFix), "Wrap"));
                }
                if (KerbalOverEngineering.Options.Settings.Enabled("Stock.PortraitHiddenCull"))
                    harmony.Patch(AccessTools.Method(typeof(KerbalPortrait), "RectContainment_OnContainmentChanged"),
                        postfix: new HarmonyMethod(typeof(PortraitFrameFix), "PartialVisible"));
                Debug.Log("[PortraitFrameFix] enabled: visible portraits every frame, hidden portraits no render; original cameras retained");
            }
            catch (Exception error)
            {
                new Harmony(Id).UnpatchAll(Id);
                Debug.LogError("[PortraitFrameFix] disabled: " + error);
            }
            Destroy(this);
        }
        static void PartialVisible(KerbalPortrait __instance, RectUtil.ContainmentLevel cLvl)
        {
            if (cLvl == RectUtil.ContainmentLevel.Partial) __instance.OnBecameVisible();
        }
        static void Wrap(object __instance, ref IEnumerator __result)
        {
            if (__result == null) return;
            var owner = (UnityEngine.Object)__instance;
            FieldInfo field = __instance is Kerbal ? CabinPortrait : EvaPortrait;
            var visibility = new Visibility(__instance, field);
            bool cull = KerbalOverEngineering.Options.Settings.Enabled("Stock.PortraitHiddenCull");
            bool fullRate = KerbalOverEngineering.Options.Settings.Enabled("Stock.PortraitFullRate");
            __result = FrameCycle.Run(__result, () => owner != null, () => !cull || visibility.Visible(), wait => fullRate && wait is WaitForSeconds);
        }
        sealed class Visibility
        {
            readonly object owner;
            readonly FieldInfo field;
            readonly Vector3[] corners = new Vector3[4];
            public Visibility(object owner, FieldInfo field) { this.owner = owner; this.field = field; }
            public bool Visible()
            {
                var portrait = field.GetValue(owner) as KerbalPortrait;
                if (portrait == null || !portrait.isActiveAndEnabled) return false;
                RawImage image = portrait.portrait;
                if (image == null || !image.isActiveAndEnabled || image.color.a <= 0 || image.canvasRenderer.GetInheritedAlpha() <= 0) return false;
                Canvas canvas = image.canvas;
                if (canvas == null || !canvas.isActiveAndEnabled) return false;
                Canvas root = canvas.rootCanvas;
                Camera camera = root.renderMode == RenderMode.ScreenSpaceOverlay ? null : root.worldCamera;
                Rect bounds = Bounds(image.rectTransform, camera);
                Rect visible = new Rect(0, 0, Screen.width, Screen.height);
                if (camera != null) visible = camera.pixelRect;
                if (!Overlap(bounds, visible)) return false;
                var detector = portrait.rectContainment;
                return detector == null || detector.container == null || Overlap(bounds, Bounds(detector.container, camera));
            }
            Rect Bounds(RectTransform transform, Camera camera)
            {
                transform.GetWorldCorners(corners);
                Vector2 min = new Vector2(float.PositiveInfinity, float.PositiveInfinity);
                Vector2 max = new Vector2(float.NegativeInfinity, float.NegativeInfinity);
                for (int i = 0; i < 4; i++)
                {
                    Vector2 point = RectTransformUtility.WorldToScreenPoint(camera, corners[i]);
                    min = Vector2.Min(min, point); max = Vector2.Max(max, point);
                }
                return Rect.MinMaxRect(min.x, min.y, max.x, max.y);
            }
            static bool Overlap(Rect a, Rect b)
            {
                return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.xMin < b.xMax && a.xMax > b.xMin && a.yMin < b.yMax && a.yMax > b.yMin;
            }
        }
    }
}
