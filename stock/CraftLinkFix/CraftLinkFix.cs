using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Craft Link Fix")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace CraftLinkFix
{
    public sealed class LinkScope
    {
        struct Entry
        {
            public Part Part;
            public int Index;
        }

        [ThreadStatic]
        static LinkScope current;
        static readonly Func<List<Part>, int> Version = MakeVersionReader();
        readonly Dictionary<uint, Entry> index = new Dictionary<uint, Entry>();
        LinkScope parent;
        List<Part> source;
        int version;
        bool topology;
        public static long IndexedLookups, IndexBuildReads, ReplacedLinearProbes, FallbackLookups;
        static Func<List<Part>, int> MakeVersionReader()
        {
            Type list = typeof(List<Part>);
            var field = AccessTools.Field(list, "_version") ?? AccessTools.Field(list, "version");
            if (field == null || field.FieldType != typeof(int))
                throw new NotSupportedException("Unsupported part-list version.");
            var method = new DynamicMethod("KOE_PartListVersion", typeof(int), new[] { list }, typeof(LinkScope), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ret);
            return (Func<List<Part>, int>)method.CreateDelegate(typeof(Func<List<Part>, int>));
        }

        public static void Validate()
        {
            var list = new List<Part>();
            int before = Version(list);
            list.Add(null);
            if (Version(list) == before)
                throw new NotSupportedException("Part-list edits do not invalidate the index.");
        }

        public static void Begin(out LinkScope __state)
        {
            __state = new LinkScope
            {
                parent = current
            };
            current = __state;
        }

        public static Exception End(Exception __exception, LinkScope __state)
        {
            if (__state != null)
            {
                current = __state.parent;
                __state.parent = null;
                __state.index.Clear();
                __state.source = null;
            }

            return __exception;
        }

        public static void EndTopology()
        {
            if (current != null)
            {
                current.topology = false;
                current.source = null;
                current.index.Clear();
            }
        }

        public static Part TopologyLookup(List<Part> parts, uint id)
        {
            if (current != null)
                current.topology = true;
            return Lookup(parts, id);
        }

        public static Part Lookup(List<Part> parts, uint id)
        {
            if (id == 0)
                return null; // Stock returns before touching even a null list.
            var scope = current;
            if (scope == null || !scope.topology || parts == null)
                return Fallback(parts, id);
            if (!ReferenceEquals(scope.source, parts) || scope.version != Version(parts))
            {
                scope.source = null;
                scope.index.Clear();
                for (int i = 0; i < parts.Count; i++)
                {
                    Part part = parts[i];
                    IndexBuildReads++;
                    if (ReferenceEquals(part, null))
                        return Fallback(parts, id);
                    // AI-generated comment: Stock scans backwards: the LAST occurrence must win for duplicate IDs.
                    if (part.craftID != 0)
                        scope.index[part.craftID] = new Entry
                        {
                            Part = part,
                            Index = i
                        };
                }

                scope.source = parts;
                scope.version = Version(parts);
            }

            Entry entry;
            if (scope.index.TryGetValue(id, out entry))
            {
                if (entry.Part.craftID != id)
                {
                    scope.source = null;
                    return Fallback(parts, id);
                }

                IndexedLookups++;
                ReplacedLinearProbes += parts.Count - entry.Index;
                return entry.Part;
            }

            // AI-generated comment: Missing IDs remain a stock scan: an unexpected in-place ID edit cannot turn a miss into stale null.
            return Fallback(parts, id);
        }

        static Part Fallback(List<Part> parts, uint id)
        {
            FallbackLookups++;
            return KSPUtil.GetPartByCraftID(parts, id);
        }
    }

    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.craft-links";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static readonly MethodInfo Find = typeof(KSPUtil).GetMethod("GetPartByCraftID", new[] { typeof(List<Part>), typeof(uint) });
        static Harmony harmony;
        public static MethodInfo ShipTarget()
        {
            return AccessTools.DeclaredMethod(typeof(ShipConstruct), "LoadShip", new[] { typeof(ConfigNode), typeof(uint), typeof(bool), typeof(string).MakeByRefType() });
        }

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.CraftLinkIndex"))
                return;
            if (harmony != null)
                return;
            if (typeof(ShipConstruct).Assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("Unsupported KSP craft loader.");
            LinkScope.Validate();
            var ship = ShipTarget();
            var attach = AccessTools.DeclaredMethod(typeof(AttachNode), "FindAttachedPart", new[] { typeof(List<Part>) });
            if (ship == null || attach == null)
                throw new NotSupportedException("Cannot find the part-link lookups expected by the craft loader.");
            // AI-generated comment: Validate every changed site before installing either patch.
            TranspileShip(PatchProcessor.GetOriginalInstructions(ship)).ToList();
            TranspileAttach(PatchProcessor.GetOriginalInstructions(attach)).ToList();
            var patcher = new Harmony(Id);
            try
            {
                patcher.Patch(
                    ship,
                    new HarmonyMethod(typeof(LinkScope), "Begin"),
                    null,
                    new HarmonyMethod(typeof(Patch), "TranspileShip"),
                    new HarmonyMethod(typeof(LinkScope), "End")
                );
                patcher.Patch(attach, null, null, new HarmonyMethod(typeof(Patch), "TranspileAttach"));
            }
            catch
            {
                patcher.Unpatch(ship, HarmonyPatchType.All, Id);
                patcher.Unpatch(attach, HarmonyPatchType.All, Id);
                throw;
            }

            harmony = patcher;
            Debug.Log("[CraftLinkFix] Loaded 1.0.0: scoped craftID index during topology linking; stock callbacks and module initialization retained.");
        }

        public static IEnumerable<CodeInstruction> TranspileShip(IEnumerable<CodeInstruction> source)
        {
            var result = new List<CodeInstruction>();
            int finds = 0, ends = 0;
            var initialize = AccessTools.DeclaredMethod(typeof(Part), "InitializeModules", Type.EmptyTypes);
            foreach (var original in source)
            {
                var c = new CodeInstruction(original);
                if (c.Calls(Find))
                {
                    c.opcode = OpCodes.Call;
                    c.operand = typeof(LinkScope).GetMethod("TopologyLookup");
                    finds++;
                }

                if (c.Calls(initialize))
                {
                    var end = new CodeInstruction(OpCodes.Call, typeof(LinkScope).GetMethod("EndTopology"));
                    end.labels.AddRange(c.labels);
                    c.labels.Clear();
                    end.blocks.AddRange(c.blocks);
                    c.blocks.Clear();
                    result.Add(end);
                    ends++;
                }

                result.Add(c);
            }

            if (finds != 2 || ends != 1)
                throw new NotSupportedException("Expected two craft link lookups and one module-initialization boundary.");
            return result;
        }

        public static IEnumerable<CodeInstruction> TranspileAttach(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            int changed = 0;
            foreach (var c in code)
                if (c.Calls(Find))
                {
                    c.opcode = OpCodes.Call;
                    c.operand = typeof(LinkScope).GetMethod("Lookup");
                    changed++;
                }

            if (changed != 1)
                throw new NotSupportedException("Cannot find the attachment lookup expected by the craft loader.");
            return code;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try
            {
                Patch.Apply();
            }
            catch (Exception ex)
            {
                Debug.LogError("[CraftLinkFix] Disabled: " + ex);
            }

            Destroy(this);
        }
    }
}
