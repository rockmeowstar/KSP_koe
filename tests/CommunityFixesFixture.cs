using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;

namespace KSPCommunityFixes
{
    public static class FixtureControl
    {
        public static bool Marker() { return true; }
        public static void SetPatch(string name, bool active)
        {
            string type, method;
            switch (name)
            {
                case "OptimizedModuleRaycasts": type = "ModuleEngines"; method = "EngineExhaustDamage"; break;
                case "ExpansionBundlePreload": type = "Expansions.ExpansionsLoader"; method = "StartLoad"; break;
                case "ScatterDistribution": type = "PQSLandControl"; method = "OnVertexBuildHeight"; break;
                case "FastLoader": type = "GameDatabase"; method = "SetupMainLoaders"; break;
                default: throw new ArgumentException(name);
            }
            var target = Assembly.Load("Assembly-CSharp").GetType(type).GetMethod(method);
            var harmony = new Harmony(name == "FastLoader" ? "KSPCommunityFixes.Performance.KSPCFFastLoaderAssetAndPartLoader" : "KSPCommunityFixes");
            if (active) harmony.Patch(target, prefix: new HarmonyMethod(typeof(FixtureControl).GetMethod("Marker")));
            else harmony.Unpatch(target, HarmonyPatchType.All, harmony.Id);
        }
    }
    public static class KSPCommunityFixes
    {
#if !UNKNOWN_API
        public static object SettingsNode { get; set; }
        public static Dictionary<Type, object> patchInstances = new Dictionary<Type, object>();
#endif
    }
}
#if !LEGACY_REPLACEMENTS
namespace KSPCommunityFixes.Performance
{
    public static class KSPCFFastLoader { public static bool IsPatchEnabled { get; set; } }
    public sealed class OptimizedModuleRaycasts { }
    public sealed class ExpansionBundlePreload { }
}
namespace KSPCommunityFixes.BugFixes { public sealed class ScatterDistribution { } }
#endif
