using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace PropellantRequestFix
{
    // AI-generated comment: The index exists only inside the stock resource-list construction call;
    // unexpected list edits or duplicate IDs fall back to the original linear search.
    public static class DeltaVLookup
    {
        const string Id = "local.kerbaloverengineering.delta-v-lookup";
        static Harmony harmony;
        static bool validate;
        [ThreadStatic] static LookupScope currentScope;
        static Func<List<DeltaVEngineResourcePartInfo>, int> readVersion;
        sealed class LookupScope
        {
            public LookupScope Parent;
            public List<DeltaVEngineResourcePartInfo> List;
            public int Version, Count;
            public readonly Dictionary<uint, DeltaVEngineResourcePartInfo> First = new Dictionary<uint, DeltaVEngineResourcePartInfo>();
            public readonly HashSet<uint> Duplicates = new HashSet<uint>();
            public bool Invalid;
        }
        public static long Queries, Entries, AliveChecks, SkippedAliveChecks, Validations, Mismatches;
        public static long IndexHits, IndexAppends, IndexFallbacks;

        static Func<List<DeltaVEngineResourcePartInfo>, int> VersionReader()
        {
            var type = typeof(List<DeltaVEngineResourcePartInfo>);
            var field = type.GetField("_version", BindingFlags.Instance | BindingFlags.NonPublic);
            if (field == null || field.FieldType != typeof(int)) return null;
            var method = new DynamicMethod("KOE_DeltaVListVersion", typeof(int), new[] { type }, typeof(DeltaVLookup), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldfld, field); il.Emit(OpCodes.Ret);
            return (Func<List<DeltaVEngineResourcePartInfo>, int>)method.CreateDelegate(typeof(Func<List<DeltaVEngineResourcePartInfo>, int>));
        }

        static void EnterScope(out object __state)
        {
            var scope = new LookupScope { Parent = currentScope };
            currentScope = scope;
            __state = scope;
        }

        static Exception LeaveScope(object __state, Exception __exception)
        {
            var scope = __state as LookupScope;
            if (scope != null) currentScope = scope.Parent;
            return __exception;
        }

        static bool TryIndexed(List<DeltaVEngineResourcePartInfo> list, uint id, out DeltaVEngineResourcePartInfo result)
        {
            result = null;
            var scope = currentScope;
            if (scope == null || readVersion == null || list == null || scope.Invalid) return false;
            int version = readVersion(list), count = list.Count;
            if (scope.List == null) { scope.List = list; scope.Version = version; scope.Count = 0; }
            else if (!ReferenceEquals(scope.List, list)) return false;
            else if (count < scope.Count || version - scope.Version != count - scope.Count)
            {
                scope.Invalid = true;
                if (validate) IndexFallbacks++;
                return false;
            }
            for (int i = scope.Count; i < count; i++)
            {
                var entry = list[i];
                if (entry == null || entry.resourcePart == null) { scope.Invalid = true; return false; }
                var part = entry.resourcePart.part;
                if (ReferenceEquals(part, null)) continue;
                uint candidateId = part.persistentId;
                if (scope.First.ContainsKey(candidateId)) scope.Duplicates.Add(candidateId);
                else scope.First.Add(candidateId, entry);
                if (validate) IndexAppends++;
            }
            scope.Count = count;
            scope.Version = version;
            if (scope.Duplicates.Contains(id)) { if (validate) IndexFallbacks++; return false; }
            DeltaVEngineResourcePartInfo candidate;
            if (scope.First.TryGetValue(id, out candidate))
            {
                var part = candidate.resourcePart.part;
                if (ReferenceEquals(part, null) || part.persistentId != id) { scope.Invalid = true; return false; }
                if (part != null) result = candidate;
            }
            if (validate) IndexHits++;
            return true;
        }
        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DeltaVLookup"))
                return;
            if (harmony != null || Environment.GetEnvironmentVariable("KOE_DELTAV_LOOKUP_MODE") == "0")
                return;
            if (typeof(Part).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                throw new NotSupportedException("Cannot patch delta-V lookups: expected KSP 1.12.5 Assembly-CSharp, found module " + typeof(Part).Module.ModuleVersionId
                    + ".");
            validate = Environment.GetEnvironmentVariable("KOE_DELTAV_LOOKUP_MODE") == "2";
            var instance = new Harmony(Id);
            try
            {
                foreach (string name in new[]
                {
                    "Get",
                    "ContainsPartInfo"
                }

                )
                {
                    MethodInfo method = AccessTools.Method(typeof(DeltaVEngineResourcePartExtensions), name);
                    var info = Harmony.GetPatchInfo(method);
                    if (info != null && info.Owners.Count != 0)
                        throw new InvalidOperationException(
                            "Cannot patch DeltaVEngineResourcePartExtensions." + name
                            + ": already patched by " + string.Join(", ", new List<string>(info.Owners).ToArray())
                            + "."
                        );
                    instance.Patch(
                        method,
                        new HarmonyMethod(typeof(DeltaVLookup), name
                        + "Prefix"),
                        validate ? new HarmonyMethod(typeof(DeltaVLookup), name
                        + "Postfix") : null
                    );
                }
                var scopedMethod = AccessTools.DeclaredMethod(typeof(DeltaVStageInfo), "GetEngineResourceParts", new[] { typeof(List<DeltaVEngineInfo>) });
                var existing = scopedMethod == null ? null : Harmony.GetPatchInfo(scopedMethod);
                try { readVersion = VersionReader(); }
                catch (Exception error)
                {
                    readVersion = null;
                    Debug.Log("[PropellantRequestFix] Scoped delta-V index unavailable: " + error.Message);
                }
                if (scopedMethod != null && readVersion != null && (existing == null || existing.Owners.Count == 0))
                {
                    instance.Patch(scopedMethod, new HarmonyMethod(typeof(DeltaVLookup), "EnterScope"), finalizer: new HarmonyMethod(typeof(DeltaVLookup), "LeaveScope"));
                    Debug.Log("[PropellantRequestFix] Scoped delta-V resource index enabled");
                }
            }
            catch
            {
                instance.UnpatchAll(Id);
                throw;
            }

            harmony = instance;
            if (validate)
                Application.quitting += Report;
            Debug.Log("[PropellantRequestFix] DeltaV lookup: " + (validate ? "shadow validation" : "ID before Unity liveness; no cache"));
        }

        public static void Remove()
        {
            if (harmony == null)
                return;
            harmony.UnpatchAll(Id);
            if (validate)
                Application.quitting -= Report;
            harmony = null;
        }

        static void Report()
        {
            Debug.Log(
                "[PropellantRequestFix] DELTAV_PARITY queries=" + Queries
                + "; validations=" + Validations
                + "; mismatches=" + Mismatches
                + "; entries=" + Entries
                + "; candidateAliveChecks=" + AliveChecks
                + "; skippedAliveChecks=" + SkippedAliveChecks
                + "; indexed=" + IndexHits + "; indexAppends=" + IndexAppends + "; indexFallbacks=" + IndexFallbacks
            );
        }

        public static DeltaVEngineResourcePartInfo Find(List<DeltaVEngineResourcePartInfo> list, DeltaVPartInfo partInfo)
        {
            if (validate)
                Queries++;
            Part target = partInfo.part;
            if (target == null)
                return null;
            uint id = target.persistentId;
            DeltaVEngineResourcePartInfo indexed;
            if (TryIndexed(list, id, out indexed)) return indexed;
            for (int i = 0; i < list.Count; i++)
            {
                DeltaVEngineResourcePartInfo entry = list[i];
                Part candidate = entry.resourcePart.part;
                if (validate)
                    Entries++;
                if (ReferenceEquals(candidate, null))
                    continue;
                if (candidate.persistentId != id)
                {
                    if (validate)
                        SkippedAliveChecks++;
                    continue;
                }

                if (validate)
                    AliveChecks++;
                if (candidate != null)
                    return entry;
            }

            return null;
        }

        static bool GetPrefix(
            List<DeltaVEngineResourcePartInfo> __0,
            DeltaVPartInfo __1,
            ref DeltaVEngineResourcePartInfo __result,
            out DeltaVEngineResourcePartInfo __state
        )
        {
            __state = Find(__0, __1);
            if (validate)
                return true;
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

        static bool ContainsPartInfoPrefix(List<DeltaVEngineResourcePartInfo> __0, DeltaVPartInfo __1, ref bool __result, out bool __state)
        {
            __state = Find(__0, __1) != null;
            if (validate)
                return true;
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
