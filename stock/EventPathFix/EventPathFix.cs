using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KSP.UI.Screens;
using UnityEngine;

namespace KoeEventPathFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        [ThreadStatic]
        static List<List<Part>> owned;
        [ThreadStatic]
        static Stack<List<Part>> free;
        [ThreadStatic]
        static Stack<bool[]> colliderMasks;
        static readonly Func<KerbalEVA, List<Collider>> Triggers = TriggerReader();
        static Func<KerbalEVA, List<Collider>> TriggerReader()
        {
            var method = new DynamicMethod("KOE_EvaTriggers", typeof(List<Collider>), new[] { typeof(KerbalEVA) }, typeof(Bootstrap), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, AccessTools.Field(typeof(KerbalEVA), "currentLadderTriggers"));
            il.Emit(OpCodes.Ret);
            return (Func<KerbalEVA, List<Collider>>)method.CreateDelegate(typeof(Func<KerbalEVA, List<Collider>>));
        }

        public void Awake()
        {
            Install("Stock.StageListReuse", typeof(KSP.UI.Screens.StageManager), "ActivateStage", "StageLists", "EnterStage", "LeaveStage");
            Install("Stock.EvaTriggerListReuse", typeof(KerbalEVA), "CheckLadderTriggers", null, "LadderTriggers", null);
            Install("Stock.PropellantGaugeLookup", typeof(ModuleEngines), "UpdatePropellantGauge", "GaugeLookup", null, null);
            Destroy(this);
        }

        static void Install(string key, Type type, string name, string transpiler, string prefix, string finalizer)
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled(key))
                return;
            var harmony = new Harmony("KerbalOverEngineering." + key);
            try
            {
                if (typeof(Part).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                    throw new NotSupportedException("The stock assembly does not match KSP 1.12.5; event patches were not installed.");
                var method = AccessTools.DeclaredMethod(type, name);
                if (method == null || Harmony.GetPatchInfo(method) != null)
                    throw new NotSupportedException("Missing/already patched " + name);
                harmony.Patch(
                    method,
                    prefix == null ? null : new HarmonyMethod(typeof(Bootstrap), prefix),
                    null,
                    transpiler == null ? null : new HarmonyMethod(typeof(Bootstrap), transpiler),
                    finalizer == null ? null : new HarmonyMethod(typeof(Bootstrap), finalizer)
                );
                Debug.Log("[KOE EventPath] Applied " + key);
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(harmony.Id);
                Debug.LogError("[KOE EventPath] Disabled " + key + ": " + ex.Message);
            }
        }

        static void EnterStage(out int __state)
        {
            if (owned == null)
                owned = new List<List<Part>>();
            __state = owned.Count;
        }

        static Exception LeaveStage(int __state, Exception __exception)
        {
            if (owned != null)
                while (owned.Count > __state)
                {
                    var list = owned[owned.Count - 1];
                    owned.RemoveAt(owned.Count - 1);
                    list.Clear();
                    free.Push(list);
                }

            return __exception;
        }

        public static List<Part> RentStage()
        {
            if (free == null)
                free = new Stack<List<Part>>();
            var list = free.Count == 0 ? new List<Part>() : free.Pop();
            owned.Add(list);
            return list;
        }

        public static IEnumerable<CodeInstruction> StageLists(IEnumerable<CodeInstruction> source)
        {
            int count = 0;
            foreach (var instruction in source)
            {
                if (instruction.opcode == OpCodes.Newobj
                    && Equals(instruction.operand, AccessTools.Constructor(typeof(List<Part>), Type.EmptyTypes)))
                {
                    var copy = new CodeInstruction(instruction);
                    copy.opcode = OpCodes.Call;
                    copy.operand = AccessTools.Method(typeof(Bootstrap), "RentStage");
                    count++;
                    yield return copy;
                }
                else
                    yield return instruction;
            }

            if (count != 1)
                throw new NotSupportedException("StageManager.ActivateStage must allocate one activation list.");
        }

        public static IEnumerable<CodeInstruction> GaugeLookup(IEnumerable<CodeInstruction> source, ILGenerator generator)
        {
            var dictionary = typeof(Dictionary<Propellant, ProtoStageIconInfo>);
            var contains = AccessTools.Method(dictionary, "ContainsKey");
            var get = AccessTools.PropertyGetter(dictionary, "Item");
            var value = generator.DeclareLocal(typeof(ProtoStageIconInfo));
            int tests = 0, reads = 0;
            foreach (var instruction in source)
            {
                if (instruction.Calls(contains))
                {
                    var address = new CodeInstruction(OpCodes.Ldloca, value);
                    address.labels.AddRange(instruction.labels);
                    address.blocks.AddRange(instruction.blocks);
                    yield return address;
                    yield return new CodeInstruction(OpCodes.Callvirt, AccessTools.Method(dictionary, "TryGetValue"));
                    tests++;
                }
                else if (instruction.Calls(get))
                {
                    var pop = new CodeInstruction(OpCodes.Pop);
                    pop.labels.AddRange(instruction.labels);
                    pop.blocks.AddRange(instruction.blocks);
                    yield return pop;
                    yield return new CodeInstruction(OpCodes.Pop);
                    yield return new CodeInstruction(OpCodes.Ldloc, value);
                    reads++;
                }
                else
                    yield return instruction;
            }

            if (tests != 1 || reads != 2)
                throw new NotSupportedException("The propellant gauge no longer uses the expected dictionary lookups.");
        }

        static bool LadderTriggers(KerbalEVA __instance)
        {
            var source = Triggers(__instance);
            if (source.Count == 0)
                return false;
            if (colliderMasks == null)
                colliderMasks = new Stack<bool[]>();
            var remove = colliderMasks.Count == 0 ? new bool[source.Count] : colliderMasks.Pop();
            if (remove.Length < source.Count)
                Array.Resize(ref remove, Math.Max(source.Count, remove.Length * 2));
            try
            {
                // AI-generated comment: Finish all potentially throwing native reads before mutating
                // the list, as stock does. Stable compaction retains null/destroyed
                // entries and duplicates of enabled colliders in original order.
                int count = source.Count;
                for (int i = 0; i < count; i++)
                    remove[i] = source[i] != null && !source[i].enabled;
                int write = 0;
                for (int read = 0; read < count; read++)
                    if (!remove[read])
                    {
                        if (write != read)
                            source[write] = source[read];
                        write++;
                    }

                if (write != count)
                    source.RemoveRange(write, count - write);
            }
            finally
            {
                colliderMasks.Push(remove);
            }

            return false;
        }
    }
}
