using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KSP Exhaust Sync Fix")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace ExhaustSyncFix
{
    // Only the physics-transform synchronization around stock exhaust queries changes.
    // Fuel, damage, raycast arguments/results processing, graphics and timestep stay stock.
    public static class Patch
    {
        const string Id = "local.ksp.exhaust-sync-fix";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static Harmony harmony;
        static float lastFixedTime = float.NaN;
        public static long SyncCount;
        public static long QueryCount;

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.ExhaustSync")) return;
            if (harmony != null) return;
            if (typeof(Part).Assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("ExhaustSyncFix supports the verified KSP 1.12.5 assembly only.");
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                if (assembly.GetName().Name == "KSPCommunityFixes")
                    throw new InvalidOperationException("KSPCommunityFixes already supplies an exhaust raycast optimization.");
            MethodInfo target = AccessTools.Method(typeof(ModuleEngines), "EngineExhaustDamage", Type.EmptyTypes);
            var instance = new Harmony(Id);
            instance.Patch(target, new HarmonyMethod(typeof(Patch), "Before"), null, null,
                new HarmonyMethod(typeof(Patch), "After"));
            harmony = instance;
            Debug.Log("[ExhaustSyncFix] Applied 1.0.0: stock exhaust queries share one transform sync per physics step.");
        }

        public static void Remove()
        {
            if (harmony == null) return;
            harmony.Unpatch(AccessTools.Method(typeof(ModuleEngines), "EngineExhaustDamage", Type.EmptyTypes),
                HarmonyPatchType.All, Id);
            harmony = null;
            lastFixedTime = float.NaN;
            Debug.Log("[ExhaustSyncFix] Removed.");
        }

        static void Before(ModuleEngines __instance, out bool __state)
        {
            __state = Physics.autoSyncTransforms;
            if (!__state || !__instance.exhaustDamage) return;
            QueryCount++;
            float now = Time.fixedTime;
            if (now != lastFixedTime)
            {
                Physics.SyncTransforms();
                lastFixedTime = now;
                SyncCount++;
            }
            Physics.autoSyncTransforms = false;
        }

        // A Harmony finalizer restores the caller's setting even after an exception.
        static Exception After(Exception __exception, bool __state)
        {
            Physics.autoSyncTransforms = __state;
            return __exception;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try { Patch.Apply(); }
            catch (Exception ex) { Debug.LogError("[ExhaustSyncFix] Disabled: " + ex.Message); }
            Destroy(this);
        }
    }
}
