using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeDeltaVReadBatchFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string PatchId = "KerbalOverEngineering.Stock.DeltaVReadBatch";
        delegate void ResourceQuery(Part part, int resourceId, ResourceFlowMode flowMode, bool simulate, out double amount, out double capacity, bool pulling);
        static readonly MethodInfo OriginalQuery = typeof(Part).GetMethod(
            "GetConnectedResourceTotals",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null,
            new[] { typeof(int), typeof(ResourceFlowMode), typeof(bool), typeof(double).MakeByRefType(), typeof(double).MakeByRefType(), typeof(bool) },
            null
        );
        static readonly ResourceQuery QueryStock = (ResourceQuery)Delegate.CreateDelegate(typeof(ResourceQuery), OriginalQuery);
        static readonly Func<ModuleEngines, ICollection> EngineAdjusters = CreateAdjusterReader();
        static MethodInfo[] watchedMethods;
        struct EngineScope
        {
            internal ReadBatch Batch;
            internal bool Suppressed;
        }

        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DeltaVReadBatch"))
            {
                Destroy(this);
                return;
            }

            var harmony = new Harmony(PatchId);
            try
            {
                Guid moduleId = typeof(Part).Module.ModuleVersionId;
                if (moduleId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                    throw new NotSupportedException("Delta-V batching requires KSP 1.12.5 Assembly-CSharp; found module " + moduleId);
                watchedMethods = FindWatchedMethods();
                var simulation = AccessTools.Method(typeof(DeltaVStageInfo), "SimulateDeltaV");
                var burn = AccessTools.Method(typeof(DeltaVEngineInfo), "CalculateBurn");
                BatchPhases(PatchProcessor.GetOriginalInstructions(simulation)).ToList();
                BatchQuery(PatchProcessor.GetOriginalInstructions(burn)).ToList();
                harmony.Patch(
                    simulation,
                    prefix: new HarmonyMethod(typeof(Bootstrap), "BeginSimulation"),
                    transpiler: new HarmonyMethod(typeof(Bootstrap), "BatchPhases"),
                    finalizer: new HarmonyMethod(typeof(Bootstrap), "EndSimulation")
                );
                harmony.Patch(
                    burn,
                    prefix: new HarmonyMethod(typeof(Bootstrap), "BeginEngine"),
                    transpiler: new HarmonyMethod(typeof(Bootstrap), "BatchQuery"),
                    finalizer: new HarmonyMethod(typeof(Bootstrap), "EndEngine")
                );
                Debug.Log("[KOE DeltaV] Reusing resource totals between CalculateBurn calls.");
            }
            catch (Exception error)
            {
                harmony.UnpatchAll(PatchId);
                Debug.LogError("[KOE DeltaV] Resource batching disabled: " + error.Message);
            }

            Destroy(this);
        }

        static Func<ModuleEngines, ICollection> CreateAdjusterReader()
        {
            var field = AccessTools.Field(typeof(ModuleEngines), "adjusterCache");
            if (field == null || !typeof(ICollection).IsAssignableFrom(field.FieldType))
                throw new MissingFieldException("ModuleEngines.adjusterCache is no longer a collection.");
            var method = new DynamicMethod("KOE_EngineAdjusters", typeof(ICollection), new[] { typeof(ModuleEngines) }, typeof(Bootstrap), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ret);
            return (Func<ModuleEngines, ICollection>)method.CreateDelegate(typeof(Func<ModuleEngines, ICollection>));
        }

        static MethodInfo[] FindWatchedMethods()
        {
            var names = new HashSet<string>
            {
                "GetConnectedResourceTotals",
                "GetOrCreateList",
                "GetFlowMode",
                "getMaxFuelFlow",
                "getFuelFlow",
                "ApplyThrottleAdjustments",
                "IsEngineDead",
                "CalculateBurn",
                "SimulateDeltaV"
            };
            return new[]
            {
                typeof(Part),
                typeof(PartSet),
                typeof(Vessel),
                typeof(Propellant),
                typeof(ModuleEngines),
                typeof(ModuleEnginesFX),
                typeof(DeltaVEngineInfo),
                typeof(DeltaVStageInfo)
            }.SelectMany(type => type.GetMethods(BindingFlags.Instance | BindingFlags.DeclaredOnly | BindingFlags.Public | BindingFlags.NonPublic)).Where(method => names.Contains(method.Name)).ToArray();
        }

        static bool HasForeignPatch()
        {
            foreach (var method in watchedMethods)
            {
                var patches = Harmony.GetPatchInfo(method);
                if (patches != null && patches.Owners.Any(owner => owner != PatchId))
                    return true;
            }

            return false;
        }

        static void BeginSimulation(out ReadBatch __state)
        {
            __state = new ReadBatch();
            // AI-generated comment: other patches may change totals or mutate fuel during CalculateBurn.
            if (HasForeignPatch())
                __state.Suppress();
        }

        static Exception EndSimulation(Exception __exception, ReadBatch __state)
        {
            if (__state != null)
                __state.Dispose();
            return __exception;
        }

        static void BeginEngine(DeltaVEngineInfo __instance, bool logMsgs, out EngineScope __state)
        {
            var engine = __instance.engine;
            bool trusted = !logMsgs
                && !ReferenceEquals(engine, null)
                && (engine.GetType() == typeof(ModuleEngines)
                || engine.GetType() == typeof(ModuleEnginesFX))
                && !ReferenceEquals(engine.part, null)
                && engine.part.GetType() == typeof(Part);
            ICollection adjusters = trusted ? EngineAdjusters(engine) : null;
            trusted = trusted && adjusters != null && adjusters.Count == 0;
            __state = new EngineScope
            {
                Batch = ReadBatch.Current,
                Suppressed = !trusted && ReadBatch.Current != null
            };
            // AI-generated comment: engine subclasses, throttle adjusters and log callbacks can run mod code.
            if (__state.Suppressed)
                __state.Batch.Suppress();
        }

        static Exception EndEngine(Exception __exception, EngineScope __state)
        {
            if (__state.Suppressed)
                __state.Batch.Resume();
            return __exception;
        }

        public static IEnumerable<CodeInstruction> BatchQuery(IEnumerable<CodeInstruction> instructions)
        {
            var result = instructions.Select(code => new CodeInstruction(code)).ToList();
            int replacements = 0;
            foreach (var code in result)
            {
                if (!code.Calls(OriginalQuery))
                    continue;
                code.opcode = OpCodes.Call;
                code.operand = AccessTools.Method(typeof(Bootstrap), "ResourceTotals");
                replacements++;
            }

            if (replacements != 1)
                throw new NotSupportedException("CalculateBurn: expected one simulated resource query; found " + replacements);
            return result;
        }

        public static IEnumerable<CodeInstruction> BatchPhases(IEnumerable<CodeInstruction> instructions)
        {
            var source = instructions.Select(code => new CodeInstruction(code)).ToList();
            int reads = source.Count(code => CallsEngine(code, "CalculateBurn"));
            int burns = source.Count(code => CallsEngine(code, "ApplyBurn"));
            if (reads != 2 || burns != 1)
                throw new NotSupportedException("SimulateDeltaV: expected two CalculateBurn calls and one ApplyBurn; found " + reads
                    + " and " + burns);
            foreach (var code in source)
            {
                var method = code.operand as MethodBase;
                bool readOnly = CallsEngine(code, "CalculateBurn") || IsListRead(method);
                // AI-generated comment: only adjacent CalculateBurn calls share totals. Any other call ends the phase.
                if (method != null && !readOnly)
                {
                    var barrier = new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(ReadBatch), "Barrier"));
                    barrier.labels.AddRange(code.labels);
                    code.labels.Clear();
                    foreach (var block in code.blocks.Where(block => block.blockType != ExceptionBlockType.EndExceptionBlock).ToArray())
                    {
                        barrier.blocks.Add(block);
                        code.blocks.Remove(block);
                    }

                    yield return barrier;
                }

                yield return code;
            }
        }

        static bool CallsEngine(CodeInstruction code, string name)
        {
            var method = code.operand as MethodInfo;
            return method != null && method.DeclaringType == typeof(DeltaVEngineInfo) && method.Name == name;
        }

        static bool IsListRead(MethodBase method)
        {
            if (method == null || method.DeclaringType == null || !method.DeclaringType.IsGenericType)
                return false;
            return method.DeclaringType.GetGenericTypeDefinition() == typeof(List<>)
                && (method.Name == "get_Count"
                || method.Name == "get_Item");
        }

        static PartSet SimulationNetwork(Part part, ResourceFlowMode mode, bool simulate)
        {
            if (!simulate || ReferenceEquals(part, null) || part.GetType() != typeof(Part))
                return null;
            switch (mode)
            {
                case ResourceFlowMode.STACK_PRIORITY_SEARCH:
                case ResourceFlowMode.STAGE_STACK_FLOW:
                case ResourceFlowMode.STAGE_STACK_FLOW_BALANCE:
                    return part.simulationCrossfeedPartSet;
                case ResourceFlowMode.ALL_VESSEL:
                case ResourceFlowMode.STAGE_PRIORITY_FLOW:
                case ResourceFlowMode.ALL_VESSEL_BALANCE:
                case ResourceFlowMode.STAGE_PRIORITY_FLOW_BALANCE:
                    // AI-generated comment: editor-wide queries use ShipConstruct, which this patch does not batch.
                    if (!ReferenceEquals(part.ship, null) && HighLogic.LoadedSceneIsEditor)
                        return null;
                    // AI-generated comment: use Unity's null check here, as Part does for a destroyed vessel.
                    return part.vessel == null ? null : part.vessel.simulationResourcePartSet;
                default:
                    return null;
            }
        }

        static void ResourceTotals(Part part, int resourceId, ResourceFlowMode mode, bool simulate, out double amount, out double capacity, bool pulling)
        {
            var network = SimulationNetwork(part, mode, simulate);
            if (!ReadBatch.TryRead(network, resourceId, pulling, simulate, out amount, out capacity))
                QueryStock(part, resourceId, mode, simulate, out amount, out capacity, pulling);
        }
    }
}
