using System;
using System.Collections;
using System.Linq;
using System.Reflection;

namespace KerbalOverEngineering.Options
{
    // No binary dependency on KSPCF or Harmony. Query actual method patches:
    // a partially failed patch group may never reach KSPCF's success registry.
    public static class CommunityFixes
    {
        static Assembly installed;
        static Assembly InstalledAssembly
        {
            get { return installed ?? (installed = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "KSPCommunityFixes")); }
        }
        static Type MainType
        {
            get
            {
                var assembly = InstalledAssembly;
                return assembly == null ? null : assembly.GetType("KSPCommunityFixes.KSPCommunityFixes");
            }
        }
        public static bool Present { get { return InstalledAssembly != null; } }
        public static bool Ready
        {
            get
            {
                var type = MainType;
                if (type == null) return !Present;
                var property = type.GetProperty("SettingsNode", BindingFlags.Static | BindingFlags.Public);
                try { return property != null && property.GetValue(null, null) != null; }
                catch { return false; }
            }
        }
        public static bool PatchApplied(string name)
        {
            switch (name)
            {
                case "OptimizedModuleRaycasts": return HasActualPatch("ModuleEngines", "EngineExhaustDamage");
                case "ExpansionBundlePreload": return HasActualPatch("Expansions.ExpansionsLoader", "StartLoad", "KSPCommunityFixes");
                case "ScatterDistribution": return HasActualPatch("PQSLandControl", "OnVertexBuildHeight");
                default: throw new NotSupportedException("Unknown compatibility target " + name);
            }
        }
        static object Member(object owner, string name)
        {
            var type = owner.GetType();
            var field = type.GetField(name, BindingFlags.Public | BindingFlags.Instance);
            if (field != null) return field.GetValue(owner);
            var property = type.GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
            if (property != null) return property.GetValue(owner, null);
            throw new NotSupportedException("Harmony patch member missing: " + name);
        }
        static bool HasActualPatch(string typeName, string methodName, string ownerId = null)
        {
            var community = InstalledAssembly;
            if (community == null) return false;
            var assemblies = AppDomain.CurrentDomain.GetAssemblies();
            var game = assemblies.FirstOrDefault(a => a.GetName().Name == "Assembly-CSharp");
            var harmony = assemblies.FirstOrDefault(a => a.GetName().Name == "0Harmony");
            var target = game == null ? null : game.GetType(typeName);
            var api = harmony == null ? null : harmony.GetType("HarmonyLib.Harmony");
            var getInfo = api == null ? null : api.GetMethod("GetPatchInfo", BindingFlags.Public | BindingFlags.Static, null, new[] { typeof(MethodBase) }, null);
            if (target == null || getInfo == null) throw new NotSupportedException("Actual Harmony patch state is unavailable");
            var methods = target.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly).Where(m => m.Name == methodName).ToArray();
            if (methods.Length == 0) throw new NotSupportedException("Compatibility method missing: " + typeName + "." + methodName);
            foreach (var method in methods)
            {
                var info = getInfo.Invoke(null, new object[] { method });
                if (info == null) continue;
                foreach (string group in new[] { "Prefixes", "Postfixes", "Transpilers", "Finalizers" })
                    foreach (object patch in (IEnumerable)Member(info, group))
                    {
                        var callback = Member(patch, "PatchMethod") as MethodInfo;
                        if (callback != null && callback.Module.Assembly == community && (ownerId == null || (string)Member(patch, "owner") == ownerId)) return true;
                    }
            }
            return false;
        }
        public static bool FastLoaderActive
        {
            get
            {
                return HasActualPatch("GameDatabase", "SetupMainLoaders");
            }
        }
        public static bool NeedsPostLoad(string key)
        {
            return key == "Stock.ExhaustSync" || key == "Stock.ExpansionBufferReuse" ||
                key == "Stock.DlcLz4Cache" || key == "Stock.DlcEarlyPreparation" ||
                key == "Stock.PqsNoisePreparation" || key == "Stock.PqsPatchStateMemo";
        }
        public static bool ReplacesStockLoader(string key)
        {
            return key == "Stock.DdsDirectUpload" || key == "Stock.DdsPrefetch" || key == "Stock.PngPipeline" ||
                key == "Stock.DatabaseChildWaitDrain" || key == "Stock.DatabaseReadyAsyncDrain" || key == "Stock.DatabaseFrameBudget";
        }
        // This is a first-run preference, not a permanent availability guard.
        // Inspect replacement capabilities in the installed build before its
        // patches initialize; an older build without that class keeps KOE on.
        // Users can opt back in if they disabled the upstream replacement.
        public static string DefaultReason(string key)
        {
            var assembly = InstalledAssembly;
            if (assembly == null) return null;
            string type = null;
            if (ReplacesStockLoader(key)) type = "Performance.KSPCFFastLoader";
            else if (key == "Stock.ExhaustSync") type = "Performance.OptimizedModuleRaycasts";
            else if (key == "Stock.ExpansionBufferReuse" || key == "Stock.DlcLz4Cache" || key == "Stock.DlcEarlyPreparation") type = "Performance.ExpansionBundlePreload";
            return type != null && assembly.GetType("KSPCommunityFixes." + type) != null ?
                "installed KSPCommunityFixes includes " + type.Substring(type.LastIndexOf('.') + 1) + "; enable KOE only if that upstream patch is disabled" : null;
        }
        public static string Reason(string key)
        {
            if (!Present) return null;
            if (!ReplacesStockLoader(key) && !NeedsPostLoad(key)) return null;
            var type = MainType;
            var registry = type == null ? null : type.GetField("patchInstances", BindingFlags.Static | BindingFlags.Public);
            var loader = type == null ? null : type.Assembly.GetType("KSPCommunityFixes.Performance.KSPCFFastLoader");
            var enabled = loader == null ? null : loader.GetProperty("IsPatchEnabled", BindingFlags.Static | BindingFlags.Public);
            if (type == null || registry == null || !typeof(IDictionary).IsAssignableFrom(registry.FieldType) || enabled == null || enabled.PropertyType != typeof(bool))
                return "KSPCommunityFixes compatibility API is unrecognized";
            if (NeedsPostLoad(key) && !Ready) return "waiting for KSPCommunityFixes post-load patch state";
            if (ReplacesStockLoader(key) && FastLoaderActive) return "KSPCommunityFixes FastLoader replaces the stock asset loader";
            if (key == "Stock.ExhaustSync" && PatchApplied("OptimizedModuleRaycasts")) return "KSPCommunityFixes OptimizedModuleRaycasts is active";
            if ((key == "Stock.ExpansionBufferReuse" || key == "Stock.DlcLz4Cache" || key == "Stock.DlcEarlyPreparation") && PatchApplied("ExpansionBundlePreload"))
                return "KSPCommunityFixes ExpansionBundlePreload replaces the stock DLC loader";
            if ((key == "Stock.PqsNoisePreparation" || key == "Stock.PqsPatchStateMemo") && PatchApplied("ScatterDistribution"))
                return "KSPCommunityFixes ScatterDistribution changes a guarded PQS callback";
            return null;
        }
    }
}
