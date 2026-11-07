using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace KoeSsrLifetime
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Deferred.SsrResourceCleanup";
        static FieldInfo camera, registry;
        static FieldInfo[] textures, commands, assets;
        public static long Released;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Deferred.SsrResourceCleanup")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                var type = AccessTools.TypeByName("Deferred.ScreenSpaceReflections");
                if (type == null) throw new NotSupportedException("Deferred SSR is not installed");
                if (AccessTools.DeclaredMethod(type, "OnDestroy") != null)
                    Debug.Log("[KOE SsrLifetime] Upstream destruction callback exists; adapter not installed");
                else
                {
                    string hash;
                    using (var sha = SHA256.Create()) using (var stream = File.OpenRead(type.Assembly.Location))
                        hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                    if (hash != "47052420db8ddad759ba68eb53fb747c8f13335f1d253529b51d79cdf3b44108"
                        && hash != "2f870bebde3b88ff0d051b816ad0b79c20b474acb96c28daa39e26e9edeed313")
                        throw new NotSupportedException("Deferred SSR resource ownership has not been reviewed for this build");
                    Prepare(type);
                    harmony.Patch(AccessTools.DeclaredMethod(type, "Start"), null, new HarmonyMethod(typeof(Bootstrap), "Attach"));
                    Debug.Log("[KOE SsrLifetime] Owned SSR resources have a destruction callback");
                }
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE SsrLifetime] Disabled: " + error.Message); }
            Destroy(this);
        }
        internal static void Prepare(Type type)
        {
            camera = Field(type, "targetCamera", typeof(Camera));
            registry = AccessTools.Field(type, "CameraToSSR");
            if (registry == null || !registry.IsStatic || !typeof(IDictionary).IsAssignableFrom(registry.FieldType))
                throw new NotSupportedException("SSR camera registry changed");
            textures = new[] { History(type, "ssrColor", typeof(RenderTexture)), History(type, "ssrHitDistance", typeof(RenderTexture)),
                History(type, "screenColor", typeof(RenderTexture)), History(type, "hiZTextures", typeof(RenderTexture)) };
            commands = new[] { History(type, "ssrCommandBuffer", typeof(CommandBuffer)), History(type, "screenCopyCommandBuffer", typeof(CommandBuffer)) };
            assets = new[] { Field(type, "combinedGBufferAndOceanNormalsAndSmoothness", typeof(RenderTexture)), Field(type, "oceanMask", typeof(RenderTexture)),
                Field(type, "ssrMaterial", typeof(Material)), Field(type, "blurMaterial", typeof(Material)), Field(type, "generateHiZMaterial", typeof(Material)), Field(type, "quadMesh", typeof(Mesh)) };
        }
        static FieldInfo Field(Type owner, string name, Type expected)
        {
            var field = AccessTools.Field(owner, name);
            if (field == null || field.FieldType != expected) throw new NotSupportedException("SSR field changed: " + name);
            return field;
        }
        static FieldInfo History(Type owner, string name, Type element)
        {
            var field = AccessTools.Field(owner, name);
            var array = field == null ? null : AccessTools.Field(field.FieldType, "array");
            if (array == null || !array.FieldType.IsArray || array.FieldType.GetArrayRank() != 3 || array.FieldType.GetElementType() != element)
                throw new NotSupportedException("SSR history changed: " + name);
            return field;
        }
        static void Attach(object __instance)
        {
            var component = __instance as Component;
            var cam = camera.GetValue(__instance) as Camera;
            if (component == null || cam == null) return;
            var map = (IDictionary)registry.GetValue(null);
            if (!ReferenceEquals(map[cam], __instance)) return;
            foreach (var existing in component.GetComponents<SsrOwner>()) if (ReferenceEquals(existing.Target, __instance)) return;
            component.gameObject.AddComponent<SsrOwner>().Target = __instance;
        }
        static Array TakeHistory(object owner, FieldInfo field)
        {
            var history = field.GetValue(owner);
            field.SetValue(owner, null);
            return history == null ? null : (Array)AccessTools.Field(field.FieldType, "array").GetValue(history);
        }
        internal static void Cleanup(object owner)
        {
            var cam = camera.GetValue(owner) as Camera;
            var map = (IDictionary)registry.GetValue(null);
            if (!ReferenceEquals(cam, null) && ReferenceEquals(map[cam], owner)) map.Remove(cam);
            var buffers = new HashSet<CommandBuffer>();
            for (int i = 0; i < commands.Length; i++)
            {
                var history = TakeHistory(owner, commands[i]);
                if (history == null) continue;
                foreach (CommandBuffer buffer in history)
                    if (buffer != null)
                    {
                        if (cam != null) cam.RemoveCommandBuffer(i == 0 ? CameraEvent.BeforeImageEffectsOpaque : CameraEvent.AfterForwardAlpha, buffer);
                        buffers.Add(buffer);
                    }
                Array.Clear(history, 0, history.Length);
            }
            foreach (var buffer in buffers) buffer.Dispose();
            var owned = new HashSet<Object>();
            foreach (var field in textures)
            {
                var history = TakeHistory(owner, field);
                if (history == null) continue;
                foreach (RenderTexture image in history) if (image != null) owned.Add(image);
                Array.Clear(history, 0, history.Length);
            }
            foreach (var field in assets)
            {
                var value = field.GetValue(owner) as Object;
                field.SetValue(owner, null);
                if (value != null) owned.Add(value);
            }
            foreach (var value in owned)
            {
                var image = value as RenderTexture;
                if (image != null) image.Release();
                Object.Destroy(value);
            }
            Released++;
        }
    }
    public sealed class SsrOwner : MonoBehaviour
    {
        internal object Target;
        void Release()
        {
            var target = Target; Target = null;
            if (target != null) Bootstrap.Cleanup(target);
        }
        public void LateUpdate()
        {
            if (Target != null && (Object)Target == null) { Release(); Destroy(this); }
        }
        public void OnDestroy() { Release(); }
    }
}
