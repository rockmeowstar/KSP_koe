using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KSP.Localization;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Propellant Request Fix")]
[assembly: AssemblyVersion("1.1.0.0")]

namespace PropellantRequestFix
{
    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.propellant-request";
        const string DeprivedTag = "#autoLOC_219085";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static readonly MethodInfo FormatMethod = AccessTools.Method(typeof(Localizer), "Format", new[] { typeof(string), typeof(string[]) });
        static readonly Func<string, string> StockFormat = FormatResource;
        static Harmony harmony;
        static Func<Dictionary<string, string>, int> tagVersion;
        static Func<Localizer, bool> showKeys, overrideLock, debugMissing;
        [ThreadStatic] static MessageCache cache;
        public static long Misses;

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.PropellantMessageCache")) return;
            if (harmony != null) return;
            if (typeof(ModuleEngines).Assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("PropellantRequestFix supports the verified KSP 1.12.5 assembly only.");
            PrepareReaders();
            MethodInfo target = AccessTools.Method(typeof(ModuleEngines), "RequestPropellant", new[] { typeof(double) });
            var instance = new Harmony(Id);
            try { instance.Patch(target, null, null, new HarmonyMethod(typeof(Patch), "Transpile")); }
            catch { instance.Unpatch(target, HarmonyPatchType.All, Id); throw; }
            harmony = instance;
            Debug.Log("[PropellantRequestFix] Applied: cache repeated deprivation messages; resource checks, requests, flameout and FX remain stock.");
        }

        public static void Remove()
        {
            DeltaVLookup.Remove();
            SimulatedTransfer.Remove();
            if (harmony == null) return;
            harmony.Unpatch(AccessTools.Method(typeof(ModuleEngines), "RequestPropellant", new[] { typeof(double) }), HarmonyPatchType.All, Id);
            harmony = null;
            cache = null;
        }

        // Same signature and evaluation order as the one replaced call; no resource-method IL is changed.
        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>(source);
            int count = 0;
            foreach (CodeInstruction instruction in code)
                if (instruction.Calls(FormatMethod)) count++;
            if (count != 1) throw new NotSupportedException("RequestPropellant localization call shape changed; refusing to patch.");
            foreach (CodeInstruction instruction in code)
                if (instruction.Calls(FormatMethod))
                {
                    var replacement = new CodeInstruction(instruction);
                    replacement.opcode = OpCodes.Call;
                    replacement.operand = AccessTools.Method(typeof(Patch), "FormatMessage");
                    yield return replacement;
                }
                else yield return instruction;
        }

        public static string FormatMessage(string template, string[] parameters)
        {
            var localizer = Localizer.Instance;
            if (template != DeprivedTag || parameters == null || parameters.Length != 1 || localizer == null ||
                tagVersion == null || showKeys(localizer) || overrideLock(localizer) || debugMissing(localizer))
                return Localizer.Format(template, parameters);
            var tags = Localizer.Tags;
            if (cache == null) cache = new MessageCache();
            cache.Observe(localizer, tags, tagVersion(tags), Localizer.CurrentLanguage,
                CultureInfo.CurrentCulture, CultureInfo.CurrentUICulture);
            return cache.Get(parameters[0], StockFormat);
        }

        static string FormatResource(string resource)
        {
            Misses++;
            return Localizer.Format(DeprivedTag, resource);
        }

        static Func<T, TResult> Reader<T, TResult>(string name)
        {
            var field = AccessTools.Field(typeof(T), name);
            if (field == null || field.FieldType != typeof(TResult))
                throw new NotSupportedException("Unsupported field layout: " + typeof(T).Name + "." + name);
            var method = new DynamicMethod("KOE_Read_" + name, typeof(TResult), new[] { typeof(T) }, typeof(Patch), true);
            var il = method.GetILGenerator(); il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, field); il.Emit(OpCodes.Ret);
            return (Func<T, TResult>)method.CreateDelegate(typeof(Func<T, TResult>));
        }

        static void PrepareReaders()
        {
            string name = AccessTools.Field(typeof(Dictionary<string, string>), "_version") != null ? "_version" : "version";
            tagVersion = Reader<Dictionary<string, string>, int>(name);
            // Validate on this runtime: an in-place translation edit must invalidate the cache.
            var test = new Dictionary<string, string>(); test["tag"] = "old";
            int previous = tagVersion(test); test["tag"] = "new";
            if (tagVersion(test) == previous) throw new NotSupportedException("Dictionary version does not track replacement.");
            showKeys = Reader<Localizer, bool>("_showKeysInGame");
            overrideLock = Reader<Localizer, bool>("_overrideMELock");
            debugMissing = Reader<Localizer, bool>("_debugWriteMissingKeysToLog");
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try { Patch.Apply(); }
            catch (Exception ex) { Debug.LogError("[PropellantRequestFix] Disabled: " + ex.Message); }
            try { DeltaVLookup.Apply(); }
            catch (Exception ex) { Debug.LogError("[PropellantRequestFix] DeltaV lookup disabled: " + ex.Message); }
            try { SimulatedTransfer.Apply(); }
            catch (Exception ex) { Debug.LogError("[PropellantRequestFix] Simulated transfer disabled: " + ex.Message); }
            Destroy(this);
        }
    }
}
