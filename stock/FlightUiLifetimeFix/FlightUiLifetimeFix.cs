using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KSP.UI;
using UnityEngine;
using Object = UnityEngine.Object;

namespace KoeFlightUiLifetime
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Stock.FlightUiLifecycle";
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.FlightUiLifecycle")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                if (typeof(UIMasterController).Module.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                    throw new NotSupportedException("UI scene cleanup has not been reviewed for this game assembly");
                var target = AccessTools.DeclaredMethod(typeof(UIMasterController), "OnSceneChange");
                var patches = Harmony.GetPatchInfo(target);
                if (patches != null)
                    foreach (string owner in patches.Owners)
                        if (owner != "local.koe.scene-benchmark") throw new NotSupportedException("UI scene cleanup already patched by " + owner);
                harmony.Patch(target, null, null, new HarmonyMethod(typeof(Bootstrap), "Rewrite"));
                Debug.Log("[KOE FlightUI] Scene-owned UI is destroyed before replacement controllers awake");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE FlightUI] Disabled: " + error.Message); }
            Destroy(this);
        }

        // AI-generated comment: This is limited to objects the original scene-change callback already destroys.
        // Ordinary gameplay destruction keeps Unity's deferred lifetime semantics.
        public static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>();
            foreach (var instruction in source) code.Add(new CodeInstruction(instruction));
            var deferred = AccessTools.DeclaredMethod(typeof(Object), "Destroy", new[] { typeof(Object) });
            var immediate = AccessTools.DeclaredMethod(typeof(Bootstrap), "DestroySceneUi");
            int count = 0;
            foreach (var instruction in code)
                if (instruction.Calls(deferred))
                {
                    instruction.opcode = OpCodes.Call;
                    instruction.operand = immediate;
                    count++;
                }
            if (count != 2) throw new NotSupportedException("Expected tooltip and canvas destruction in the scene-change callback");
            return code;
        }

        static void DestroySceneUi(Object value)
        {
            if (value != null) Object.DestroyImmediate(value);
        }
    }
}
