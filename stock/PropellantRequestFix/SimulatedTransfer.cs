using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace PropellantRequestFix
{
    public static class SimulatedTransfer
    {
        const string Id = "local.kerbaloverengineering.simulated-transfer";
        static Harmony harmony;
        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.SimulatedTransfer"))
                return;
            if (harmony != null || Environment.GetEnvironmentVariable("KOE_SIMULATED_TRANSFER_DISABLE") == "1")
                return;
            if (typeof(Part).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                throw new NotSupportedException("Cannot patch simulated fuel transfer: expected KSP 1.12.5 Assembly-CSharp, found module " + typeof(Part).Module.ModuleVersionId
                    + ".");
            MethodInfo method = Target();
            var info = Harmony.GetPatchInfo(method);
            if (info != null && info.Owners.Count != 0)
                throw new InvalidOperationException("Cannot patch Part.TransferResource: already patched by " + string.Join(", ", new List<string>(info.Owners).ToArray())
                    + ".");
            var instance = new Harmony(Id);
            try
            {
                instance.Patch(method, transpiler: new HarmonyMethod(typeof(SimulatedTransfer), "Transpile"));
            }
            catch
            {
                instance.UnpatchAll(Id);
                throw;
            }

            harmony = instance;
            Debug.Log("[PropellantRequestFix] Simulated transfer: skip four unreachable heat-transfer liveness gates; amounts/events remain stock.");
        }

        public static MethodInfo Target()
        {
            return AccessTools.Method(typeof(Part), "TransferResource", new[] { typeof(PartResource), typeof(double), typeof(Part), typeof(bool) });
        }

        public static void Remove()
        {
            if (harmony == null)
                return;
            harmony.UnpatchAll(Id);
            harmony = null;
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> input)
        {
            var code = new List<CodeInstruction>(input);
            var starts = new Dictionary<int, Label>();
            MethodInfo different = AccessTools.Method(typeof(UnityEngine.Object), "op_Inequality", new[] { typeof(UnityEngine.Object), typeof(UnityEngine.Object) });
            for (int i = 2; i + 7 < code.Count; i++)
            {
                if (!code[i].Calls(different))
                    continue;
                if (code[i - 2].opcode != OpCodes.Ldarg_3
                    || code[i - 1].opcode != OpCodes.Ldarg_0
                    || code[i + 1].opcode != OpCodes.Dup
                    || code[i + 2].opcode != OpCodes.Pop
                    || code[i + 3].opcode != OpCodes.Brfalse_S
                    || code[i + 6].opcode != OpCodes.Ldarg_S
                    || Convert.ToInt32(code[i + 6].operand) != 4
                    || code[i + 7].opcode != OpCodes.Brtrue_S
                    || code[i - 2].labels.Count != 0
                    || code[i - 2].blocks.Count != 0)
                    throw new NotSupportedException("Part.TransferResource no longer has the expected heat-transfer branch.");
                Label whenSame = (Label)code[i + 3].operand, whenSimulated = (Label)code[i + 7].operand;
                int samePartExitIndex = code.FindIndex(c => c.labels.Contains(whenSame));
                int simulationExitIndex = code.FindIndex(c => c.labels.Contains(whenSimulated));
                if (samePartExitIndex <= i + 7 || samePartExitIndex != simulationExitIndex)
                    throw new NotSupportedException("Heat gates no longer share their exact exit.");
                starts.Add(i - 2, whenSimulated);
            }

            if (starts.Count != 4)
                throw new NotSupportedException("Expected four stock heat gates.");
            for (int i = 0; i < code.Count; i++)
            {
                Label exit;
                if (starts.TryGetValue(i, out exit))
                {
                    yield return new CodeInstruction(OpCodes.Ldarg_S, (byte)4);
                    yield return new CodeInstruction(OpCodes.Brtrue, exit);
                }

                yield return code[i];
            }
        }
    }
}
