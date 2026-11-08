using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeScattererShadowLifetime
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Scatterer.ShadowCopyLifetime";
        static FieldInfo buffer, light, added, instances;
        static MethodInfo sceneCallback, removeSceneCallback;
        public static long ReleasedBuffers, DetachedOwners;

        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Scatterer.ShadowCopyLifetime")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                var type = AccessTools.TypeByName("Scatterer.ShadowMapCopier");
                if (type == null) throw new NotSupportedException("Scatterer shadow copier is not installed");
                string hash;
                using (var sha = SHA256.Create()) using (var stream = File.OpenRead(type.Assembly.Location))
                    hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                if (hash != ReviewedAssemblyHash)
                    throw new NotSupportedException("Scatterer shadow-copy ownership has not been reviewed for this build");
                Prepare(type);
                harmony.Patch(AccessTools.DeclaredMethod(type, "CreateTextureCopyCB"), new HarmonyMethod(typeof(Bootstrap), "BeforeReplace"));
                harmony.Patch(AccessTools.DeclaredMethod(type, "OnDestroy"), finalizer: new HarmonyMethod(typeof(Bootstrap), "AfterDestroy"));
                Debug.Log("[KOE ScattererShadowLifetime] Replaced buffers are released; destroyed owners leave the scene event and registry");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE ScattererShadowLifetime] Disabled: " + error.Message); }
            Destroy(this);
        }

        internal const string ReviewedAssemblyHash = "27d02617cd4ff0b8b105c00b8ad9cce7251955bc26eee4bf1451bbd05390a9b0";

        internal static void Prepare(Type type)
        {
            buffer = RequireField(type, "textureCopyBuffer", typeof(CommandBuffer));
            light = RequireField(type, "m_Light", typeof(Light));
            added = RequireField(type, "commandBufferAdded", typeof(bool));
            instances = AccessTools.Field(type, "instances");
            if (instances == null || !instances.IsStatic || !typeof(IDictionary).IsAssignableFrom(instances.FieldType))
                throw new NotSupportedException("Scatterer copier registry changed");
            sceneCallback = AccessTools.DeclaredMethod(type, "RecreateForSceneChange", new[] { typeof(GameScenes) });
            var create = AccessTools.DeclaredMethod(type, "CreateTextureCopyCB");
            var destroy = AccessTools.DeclaredMethod(type, "OnDestroy");
            if (sceneCallback == null || create == null || destroy == null || create.GetParameters().Length != 0 || destroy.GetParameters().Length != 0)
                throw new NotSupportedException("Scatterer copier lifecycle changed");
            removeSceneCallback = GameEvents.onGameSceneLoadRequested.GetType().GetMethod("Remove");
            if (removeSceneCallback == null || removeSceneCallback.GetParameters().Length != 1)
                throw new NotSupportedException("Scene event callback changed");
        }

        static FieldInfo RequireField(Type type, string name, Type expected)
        {
            var field = AccessTools.Field(type, name);
            if (field == null || field.IsStatic || field.FieldType != expected)
                throw new NotSupportedException("Scatterer field changed: " + name);
            return field;
        }

        internal static void BeforeReplace(object __instance)
        {
            var previous = buffer.GetValue(__instance) as CommandBuffer;
            if (previous == null) return;
            var ownerLight = light.GetValue(__instance) as Light;
            if (ownerLight != null) ownerLight.RemoveCommandBuffer(LightEvent.AfterShadowMap, previous);
            added.SetValue(__instance, false);
            buffer.SetValue(__instance, null);
            previous.Dispose();
            ReleasedBuffers++;
        }

        internal static Exception AfterDestroy(object __instance, Exception __exception)
        {
            // Preserve the original exception if upstream destruction failed.
            try
            {
                var owner = __instance as MonoBehaviour;
                if (!ReferenceEquals(owner, null)) owner.StopAllCoroutines();
                var callbackType = removeSceneCallback.GetParameters()[0].ParameterType;
                var callback = Delegate.CreateDelegate(callbackType, __instance, sceneCallback);
                removeSceneCallback.Invoke(GameEvents.onGameSceneLoadRequested, new object[] { callback });
                var registry = (IDictionary)instances.GetValue(null);
                var ownerLight = light.GetValue(__instance);
                if (ownerLight != null && ReferenceEquals(registry[ownerLight], __instance)) registry.Remove(ownerLight);
                BeforeReplace(__instance);
                DetachedOwners++;
            }
            catch (Exception error) { Debug.LogError("[KOE ScattererShadowLifetime] Cleanup failed: " + error.Message); }
            return __exception;
        }
    }
}
