using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace PropellantRequestFix
{
    // No cache: list edits, persistent-ID edits and Unity destruction are observed
    // on every lookup. Check Unity liveness only for candidates with the right ID.
    public static class DeltaVLookup
    {
        const string Id = "local.kerbaloverengineering.delta-v-lookup";
        static Harmony harmony;
        static bool validate;
        public static long Queries, Entries, AliveChecks, SkippedAliveChecks, Validations, Mismatches;

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DeltaVLookup")) return;
            if (harmony != null || Environment.GetEnvironmentVariable("KOE_DELTAV_LOOKUP_MODE") == "0") return;
            if (typeof(Part).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                throw new NotSupportedException("DeltaV lookup supports the verified KSP 1.12.5 assembly only.");
            validate = Environment.GetEnvironmentVariable("KOE_DELTAV_LOOKUP_MODE") == "2";
            var instance = new Harmony(Id);
            try
            {
                foreach (string name in new[] { "Get", "ContainsPartInfo" })
                {
                    MethodInfo method = AccessTools.Method(typeof(DeltaVEngineResourcePartExtensions), name);
                    var info = Harmony.GetPatchInfo(method);
                    if (info != null && info.Owners.Count != 0)
                        throw new InvalidOperationException("DeltaV lookup already patched by another owner.");
                    instance.Patch(method, new HarmonyMethod(typeof(DeltaVLookup), name + "Prefix"),
                        validate ? new HarmonyMethod(typeof(DeltaVLookup), name + "Postfix") : null);
                }
            }
            catch
            {
                instance.UnpatchAll(Id);
                throw;
            }
            harmony = instance;
            if (validate) Application.quitting += Report;
            Debug.Log("[PropellantRequestFix] DeltaV lookup: " + (validate ? "shadow validation" : "ID before Unity liveness; no cache"));
        }

        public static void Remove()
        {
            if (harmony == null) return;
            harmony.UnpatchAll(Id);
            if (validate) Application.quitting -= Report;
            harmony = null;
        }

        static void Report()
        {
            Debug.Log("[PropellantRequestFix] DELTAV_PARITY queries=" + Queries + "; validations=" + Validations +
                "; mismatches=" + Mismatches + "; entries=" + Entries + "; candidateAliveChecks=" + AliveChecks +
                "; skippedAliveChecks=" + SkippedAliveChecks);
        }

        public static DeltaVEngineResourcePartInfo Find(List<DeltaVEngineResourcePartInfo> list, DeltaVPartInfo partInfo)
        {
            if (validate) Queries++;
            Part target = partInfo.part;
            if (target == null) return null;
            uint id = target.persistentId;
            for (int i = 0; i < list.Count; i++)
            {
                DeltaVEngineResourcePartInfo entry = list[i];
                Part candidate = entry.resourcePart.part;
                if (validate) Entries++;
                if (ReferenceEquals(candidate, null)) continue;
                if (candidate.persistentId != id)
                {
                    if (validate) SkippedAliveChecks++;
                    continue;
                }
                if (validate) AliveChecks++;
                if (candidate != null) return entry;
            }
            return null;
        }

        static bool GetPrefix(List<DeltaVEngineResourcePartInfo> __0, DeltaVPartInfo __1,
            ref DeltaVEngineResourcePartInfo __result, out DeltaVEngineResourcePartInfo __state)
        {
            __state = Find(__0, __1);
            if (validate) return true;
            __result = __state;
            return false;
        }

        static void GetPostfix(DeltaVEngineResourcePartInfo __result, DeltaVEngineResourcePartInfo __state)
        {
            Validations++;
            if (!ReferenceEquals(__result, __state))
            {
                Mismatches++;
                throw new InvalidOperationException("DeltaV Get parity mismatch.");
            }
        }

        static bool ContainsPartInfoPrefix(List<DeltaVEngineResourcePartInfo> __0, DeltaVPartInfo __1,
            ref bool __result, out bool __state)
        {
            __state = Find(__0, __1) != null;
            if (validate) return true;
            __result = __state;
            return false;
        }

        static void ContainsPartInfoPostfix(bool __result, bool __state)
        {
            Validations++;
            if (__result != __state)
            {
                Mismatches++;
                throw new InvalidOperationException("DeltaV Contains parity mismatch.");
            }
        }
    }
}
