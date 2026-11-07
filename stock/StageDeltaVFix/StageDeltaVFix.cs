using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using HarmonyLib;
using UnityEngine;

namespace KoeStageDeltaV
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Stock.DeltaVResetIndex";
        [ThreadStatic] static Scope current;
        static readonly FieldInfo Info = AccessTools.Field(typeof(VesselDeltaV), "_partInfo");
        static readonly Func<List<DeltaVPartInfo>, int> Version = VersionReader();
        static bool audit;
        public static long Searches, Builds, Checks, Mismatches;
        sealed class PartReferenceComparer : IEqualityComparer<Part>
        {
            internal static readonly PartReferenceComparer Instance = new PartReferenceComparer();
            public bool Equals(Part left, Part right) { return ReferenceEquals(left, right); }
            public int GetHashCode(Part part) { return RuntimeHelpers.GetHashCode(part); }
        }
        sealed class Scope
        {
            internal Scope Parent;
            internal List<Part> Parts;
            internal HashSet<Part> Membership;
            internal List<DeltaVPartInfo> Infos;
            internal int Version = -1;
            internal Dictionary<Part, int> First;
        }
        static Func<List<DeltaVPartInfo>, int> VersionReader()
        {
            var f = AccessTools.Field(typeof(List<DeltaVPartInfo>), "_version") ?? AccessTools.Field(typeof(List<DeltaVPartInfo>), "version");
            if (f == null || f.FieldType != typeof(int))
                throw new NotSupportedException("Unknown list version field");
            var m = new DynamicMethod("KOE_ResetListVersion", typeof(int), new[] { typeof(List<DeltaVPartInfo>) }, typeof(Bootstrap), true);
            var il = m.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, f);
            il.Emit(OpCodes.Ret);
            return (Func<List<DeltaVPartInfo>, int>)m.CreateDelegate(typeof(Func<List<DeltaVPartInfo>, int>));
        }
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DeltaVResetIndex"))
            {
                Destroy(this);
                return;
            }
            var h = new Harmony(Id);
            try
            {
                var method = AccessTools.DeclaredMethod(typeof(VesselDeltaV), "ResetPartInfo");
                if (typeof(Part).Module.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                    throw new NotSupportedException("Unsupported stock delta-V implementation");
                foreach (var m in new[] { method, AccessTools.Method(typeof(DeltaVPartInfo), "CalculateMassValues"), AccessTools.Method(typeof(DeltaVPartInfo), "CalculateStagingValues") })
                    if (Harmony.GetPatchInfo(m) != null)
                        throw new NotSupportedException("Delta-V reset or calculation already patched: " + m.Name);
                audit = Environment.GetEnvironmentVariable("KOE_STAGE_INDEX_AUDIT") == "1";
                h.Patch(method, new HarmonyMethod(typeof(Bootstrap), "Enter"), null, new HarmonyMethod(typeof(Bootstrap), "Rewrite"), new HarmonyMethod(typeof(Bootstrap), "Leave"));
                Application.quitting += Report;
                Debug.Log("[KOE StageDeltaV] Indexed reset enabled; audit=" + audit);
            }
            catch (Exception error)
            {
                h.UnpatchAll(Id);
                Debug.LogError("[KOE StageDeltaV] Disabled: " + error.Message);
            }
            Destroy(this);
        }
        static void Report()
        {
            Debug.Log("[KOE StageDeltaV] searches=" + Searches + " builds=" + Builds + " checks=" + Checks + " mismatches=" + Mismatches);
        }
        static void Enter(List<Part> __0, out object __state)
        {
            var s = new Scope { Parent = current, Parts = __0 };
            current = s;
            __state = s;
        }
        static Exception Leave(object __state, Exception __exception)
        {
            var scope = __state as Scope;
            if (scope != null)
                current = scope.Parent;
            return __exception;
        }
        public static bool Contains(List<Part> list, Part part)
        {
            var s = current;
            if (s == null || !ReferenceEquals(list, s.Parts))
                return list.Contains(part);
            // AI-generated comment: Membership is used only in the initial prune loop, before stock callbacks run.
            if (s.Membership == null)
                s.Membership = new HashSet<Part>(list);
            bool result = s.Membership.Contains(part);
            if (audit)
            {
                Checks++;
                if (result != list.Contains(part))
                {
                    Mismatches++;
                    throw new InvalidOperationException("Part membership differs");
                }
            }
            return result;
        }
        public static int Find(List<DeltaVPartInfo> list, Part part)
        {
            Searches++;
            var s = current;
            if (s == null)
                return Linear(list, part);
            int version = Version(list);
            if (!ReferenceEquals(s.Infos, list) || s.Version != version)
            {
                // AI-generated comment: Stock Unity equality compares two nonnull wrappers by reference, even if a subclass overrides Equals.
                s.First = new Dictionary<Part, int>(PartReferenceComparer.Instance);
                for (int i = 0; i < list.Count; i++)
                {
                    if (!ReferenceEquals(list[i].part, null) && !s.First.ContainsKey(list[i].part))
                        s.First.Add(list[i].part, i);
                }
                s.Infos = list;
                s.Version = version;
                Builds++;
            }
            int found;
            if (ReferenceEquals(part, null) || !s.First.TryGetValue(part, out found))
                found = -1;
            if (found >= 0 && list[found].part != part)
            {
                s.Version = -1;
                found = Linear(list, part);
            }
            if (audit)
            {
                Checks++;
                int expected = Linear(list, part);
                if (expected != found)
                {
                    Mismatches++;
                    throw new InvalidOperationException("First part match differs");
                }
            }
            return found;
        }
        static int Linear(List<DeltaVPartInfo> list, Part part)
        {
            for (int i = 0; i < list.Count; i++)
            {
                if (list[i].part == part)
                    return i;
            }
            return -1;
        }
        public static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> input)
        {
            var il = input.Select(x => new CodeInstruction(x)).ToList();
            // AI-generated comment: Keep the stock prune, callbacks, additions and staging tail; replace only its searches.
            var contains = AccessTools.Method(typeof(List<Part>), "Contains");
            int count = 0;
            foreach (var x in il)
            {
                if (x.Calls(contains))
                {
                    x.opcode = OpCodes.Call;
                    x.operand = AccessTools.Method(typeof(Bootstrap), "Contains");
                    count++;
                }
            }
            if (count != 1 || il.Count < 160 || il[68].opcode != OpCodes.Ldc_I4_0 || il[69].opcode != OpCodes.Stloc_3
                || il[83].operand as MethodInfo != AccessTools.Method(typeof(UnityEngine.Object), "op_Equality")
                || il[89].opcode != OpCodes.Ldarg_0 || il[127].opcode != OpCodes.Ldloc_2
                || il[103].operand as MethodInfo != AccessTools.Method(typeof(DeltaVPartInfo), "CalculateMassValues"))
                throw new NotSupportedException("Delta-V reset search layout differs");
            var exit = il[113].operand;
            var replacement = new[]{new CodeInstruction(OpCodes.Ldarg_0),new CodeInstruction(OpCodes.Ldfld,Info),new CodeInstruction(OpCodes.Ldarg_1),new CodeInstruction(OpCodes.Ldloc_1),
                new CodeInstruction(OpCodes.Callvirt,AccessTools.PropertyGetter(typeof(List<Part>),"Item")),new CodeInstruction(OpCodes.Call,AccessTools.Method(typeof(Bootstrap),"Find")),
                new CodeInstruction(OpCodes.Stloc_3),new CodeInstruction(OpCodes.Ldloc_3),new CodeInstruction(OpCodes.Ldc_I4_0),new CodeInstruction(OpCodes.Blt,exit)};
            var removed = new HashSet<Label>(il.Skip(68).Take(21).Concat(il.Skip(114).Take(13)).SelectMany(x => x.labels));
            for (int i = 0; i < il.Count; i++)
            {
                if (i >= 68 && i < 89 || i >= 114 && i < 127)
                    continue;
                var label = il[i].operand;
                if (label is Label && removed.Contains((Label)label))
                    throw new NotSupportedException("External branch enters replaced search");
                var labels = label as Label[];
                if (labels != null && labels.Any(removed.Contains))
                    throw new NotSupportedException("External switch enters replaced search");
            }
            if (il.Skip(68).Take(59).Any(x => x.blocks.Count != 0))
                throw new NotSupportedException("Exception region crosses reset search");
            il.RemoveRange(114, 13);
            il.RemoveRange(68, 21);
            il.InsertRange(68, replacement);
            return il;
        }
    }
}
