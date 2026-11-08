using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeScattererScreenCopy
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Scatterer.ScreenCopyReuse";
        static FieldInfo camera, buffer, hdr, reflection, attached, registry;
        sealed class State
        {
            internal object Camera, Buffer;
            internal bool Hdr, Reflection;
        }
        static readonly ConditionalWeakTable<object, State> built = new ConditionalWeakTable<object, State>();
        public static long SkippedRebuilds, ReleasedBuffers;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Scatterer.ScreenCopyReuse")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                var type = AccessTools.TypeByName("Scatterer.ScreenCopyCommandBuffer");
                if (type == null) throw new NotSupportedException("Scatterer screen copier is not installed");
                string hash;
                using (var sha = SHA256.Create()) using (var stream = File.OpenRead(type.Assembly.Location))
                    hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                if (hash != "27d02617cd4ff0b8b105c00b8ad9cce7251955bc26eee4bf1451bbd05390a9b0")
                    throw new NotSupportedException("Scatterer screen-copy command construction has not been reviewed for this build");
                Prepare(type);
                harmony.Patch(AccessTools.DeclaredMethod(type, "Reinit"), new HarmonyMethod(typeof(Bootstrap), "BeforeReinit"), new HarmonyMethod(typeof(Bootstrap), "AfterReinit"));
                harmony.Patch(AccessTools.DeclaredMethod(type, "OnDestroy"), finalizer: new HarmonyMethod(typeof(Bootstrap), "AfterDestroy"));
                Debug.Log("[KOE ScattererScreenCopy] Identical command lists are reused; original texture format is preserved");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE ScattererScreenCopy] Disabled: " + error.Message); }
            Destroy(this);
        }
        internal static void Prepare(Type type)
        {
            camera = Field(type, "targetCamera", typeof(Camera));
            buffer = Field(type, "screenCopyCommandBuffer", typeof(CommandBuffer));
            hdr = Field(type, "hdrEnabled", typeof(bool));
            reflection = Field(type, "reflectionProbeMode", typeof(bool));
            attached = Field(type, "isEnabled", typeof(bool));
            registry = AccessTools.Field(type, "CameraToCommandBufferHandler");
            if (registry == null || !registry.IsStatic || !typeof(IDictionary).IsAssignableFrom(registry.FieldType)
                || AccessTools.DeclaredMethod(type, "Reinit") == null || AccessTools.DeclaredMethod(type, "OnDestroy") == null)
                throw new NotSupportedException("Scatterer screen-copy lifecycle changed");
        }
        static FieldInfo Field(Type type, string name, Type expected)
        {
            var field = AccessTools.Field(type, name);
            if (field == null || field.IsStatic || field.FieldType != expected) throw new NotSupportedException("Screen-copy field changed: " + name);
            return field;
        }
        internal static bool BeforeReinit(object __instance, out bool __state,
            Camera ___targetCamera, CommandBuffer ___screenCopyCommandBuffer,
            bool ___hdrEnabled, bool ___reflectionProbeMode, bool ___isEnabled)
        {
            State previous;
            bool same = !___isEnabled && built.TryGetValue(__instance, out previous)
                && ReferenceEquals(previous.Camera, ___targetCamera)
                && ReferenceEquals(previous.Buffer, ___screenCopyCommandBuffer)
                && previous.Hdr == ___hdrEnabled
                && previous.Reflection == ___reflectionProbeMode;
            __state = !same;
            if (same) SkippedRebuilds++;
            return !same;
        }
        internal static void AfterReinit(object __instance, bool __state,
            Camera ___targetCamera, CommandBuffer ___screenCopyCommandBuffer,
            bool ___hdrEnabled, bool ___reflectionProbeMode)
        {
            if (!__state) return;
            built.Remove(__instance);
            built.Add(__instance, new State { Camera = ___targetCamera, Buffer = ___screenCopyCommandBuffer,
                Hdr = ___hdrEnabled, Reflection = ___reflectionProbeMode });
        }
        internal static Exception AfterDestroy(object __instance, Exception __exception)
        {
            try
            {
                built.Remove(__instance);
                var cam = camera.GetValue(__instance) as Camera;
                var map = (IDictionary)registry.GetValue(null);
                if (!ReferenceEquals(cam, null) && ReferenceEquals(map[cam], __instance)) map.Remove(cam);
                var commands = buffer.GetValue(__instance) as CommandBuffer;
                if (commands != null)
                {
                    if (cam != null) cam.RemoveCommandBuffer(CameraEvent.BeforeImageEffectsOpaque, commands);
                    buffer.SetValue(__instance, null);
                    attached.SetValue(__instance, false);
                    commands.Dispose(); ReleasedBuffers++;
                }
            }
            catch (Exception error) { Debug.LogError("[KOE ScattererScreenCopy] Cleanup failed: " + error.Message); }
            return __exception;
        }
    }
}
