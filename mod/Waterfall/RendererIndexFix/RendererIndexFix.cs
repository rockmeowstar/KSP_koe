using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using HarmonyLib;
using UnityEngine;

namespace KoeWaterfallRendererIndex
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        static readonly ConditionalWeakTable<IList<Renderer>, HashSet<Renderer>> indices = new ConditionalWeakTable<IList<Renderer>, HashSet<Renderer>>();
        static readonly ConditionalWeakTable<IList<Renderer>, HashSet<Renderer>>.CreateValueCallback createIndex = Index;
        public void Awake()
        {
            const string key = "Waterfall.RendererMembershipIndex";
            if (!KerbalOverEngineering.Options.Settings.Enabled(key))
            {
                Destroy(this);
                return;
            }

            var harmony = new Harmony("KerbalOverEngineering." + key);
            try
            {
                var type = AccessTools.TypeByName("Waterfall.ModuleWaterfallFX");
                if (type == null)
                    throw new NotSupportedException("Waterfall is absent");
                var method = AccessTools.DeclaredMethod(type, "GatherRenderers", Type.EmptyTypes);
                if (method == null || Harmony.GetPatchInfo(method) != null)
                    throw new NotSupportedException("Waterfall GatherRenderers is missing, has a different signature, or is already patched.");
                harmony.Patch(method, null, null, new HarmonyMethod(typeof(Bootstrap), "Transpile"));
                Debug.Log("[KOE Waterfall] Ordered renderer list retained; membership indexed per gather");
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(harmony.Id);
                Debug.LogError("[KOE Waterfall] Disabled: " + ex.Message);
            }

            Destroy(this);
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            int edits = 0;
            var code = new List<CodeInstruction>(source);
            for (int i = 0; i < code.Count; i++)
            {
                var instruction = code[i];
                var method = instruction.operand as MethodInfo;
                if (method != null
                    && method.Name == "AddUnique"
                    && method.IsGenericMethod
                    && method.GetGenericArguments()[0] == typeof(Renderer))
                {
                    if (method.ReturnType != typeof(Renderer) || i + 1 >= code.Count || code[i + 1].opcode != OpCodes.Pop)
                        throw new NotSupportedException("GatherRenderers must discard the AddUnique return value after each call.");
                    var copy = new CodeInstruction(instruction);
                    copy.opcode = OpCodes.Call;
                    copy.operand = AccessTools.Method(typeof(Bootstrap), "AddUnique");
                    edits++;
                    yield return copy;
                }
                else
                    yield return instruction;
            }

            if (edits != 1)
                throw new NotSupportedException("Cannot find the AddUnique call in Waterfall GatherRenderers.");
        }

        static HashSet<Renderer> Index(IList<Renderer> list)
        {
            return new HashSet<Renderer>(list);
        }

        public static Renderer AddUnique(IList<Renderer> list, Renderer value)
        {
            // AI-generated comment: List-local ownership handles reentrant gathers. The weak key avoids
            // retaining renderer lists or destroyed parts after their gather ends.
            if (indices.GetValue(list, createIndex).Add(value))
                list.Add(value);
            return value; // verified call site discards the return value
        }
    }
}
