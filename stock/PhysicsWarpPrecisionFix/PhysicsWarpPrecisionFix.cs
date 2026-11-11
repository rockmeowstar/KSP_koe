using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KerbalOverEngineering.Options;
using UnityEngine;

namespace KoePhysicsWarpPrecision
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.PhysicsWarpPrecision";
        public void Awake()
        {
            if (!Settings.Enabled("Stock.PhysicsWarpPrecision")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            PhysicsWarpPrecision.PatchesReady = false;
            try
            {
                var sas = AccessTools.DeclaredMethod(typeof(VesselAutopilot).GetNestedType("VesselSAS",
                    BindingFlags.Public | BindingFlags.NonPublic), "AutoTuneScalar");
                var thermal = AccessTools.DeclaredMethod(typeof(FlightIntegrator), "UpdateThermodynamics");
                var thermalFrame = AccessTools.DeclaredMethod(typeof(FlightIntegrator), "Update");
                var thermalTick = AccessTools.DeclaredMethod(typeof(FlightIntegrator), "FixedUpdate");
                var render = AccessTools.DeclaredMethod(typeof(Vessel), "Update");
                var input = AccessTools.DeclaredMethod(typeof(Vessel), "FeedInputFeed");
                var warp = AccessTools.DeclaredMethod(typeof(TimeWarp), "SetRate", new[] {typeof(int), typeof(bool), typeof(bool)});
                if (sas == null || thermal == null || thermalFrame == null || thermalTick == null || render == null || input == null || warp == null)
                    throw new NotSupportedException("Stock physics methods changed");
                RequireRateCalls(sas, 7);
                RequireRateCalls(thermal, 2);
                CheckRewrite(sas, NormalizeRate);
                CheckRewrite(thermal, NormalizeRate);
                CheckRewrite(render, MoveAutopilot);
                CheckRewrite(thermalTick, MoveThermalInputs);
                EvaInputFix.Prepare();
                KoeFarPhysicsPrecision.Patches.Prepare();
                harmony.Patch(sas, transpiler: new HarmonyMethod(typeof(Bootstrap), "NormalizeRate"));
                harmony.Patch(thermal, transpiler: new HarmonyMethod(typeof(Bootstrap), "NormalizeRate"));
                harmony.Patch(thermalFrame, prefix: new HarmonyMethod(typeof(Bootstrap), "BeforeThermalFrame"));
                harmony.Patch(thermalTick, transpiler: new HarmonyMethod(typeof(Bootstrap), "MoveThermalInputs"));
                harmony.Patch(render, transpiler: new HarmonyMethod(typeof(Bootstrap), "MoveAutopilot"));
                harmony.Patch(input, prefix: new HarmonyMethod(typeof(Bootstrap), "BeforeInput"));
                harmony.Patch(warp, prefix: new HarmonyMethod(typeof(Bootstrap), "BeforeWarp"));
                harmony.Patch(EvaInputFix.Target, transpiler: new HarmonyMethod(typeof(EvaInputFix), "Rewrite"));
                KoeFarPhysicsPrecision.Patches.Install(harmony);
                PhysicsWarpPrecision.PatchesReady = true;
                Debug.Log("[KOE PhysicsWarpPrecision] Constant-step SAS, thermal integration, controls and FAR publication ready");
            }
            catch (Exception error)
            {
                harmony.UnpatchAll(Id);
                Debug.LogError("[KOE PhysicsWarpPrecision] Disabled: " + error.Message);
            }
            Destroy(this);
        }

        internal static void CheckRewrite(MethodInfo method, Func<IEnumerable<CodeInstruction>, IEnumerable<CodeInstruction>> rewrite)
        {
            foreach (var instruction in rewrite(PatchProcessor.GetOriginalInstructions(method))) { }
        }

        static void RequireRateCalls(MethodInfo method, int expected)
        {
            var rate = AccessTools.PropertyGetter(typeof(TimeWarp), "CurrentRate");
            int count = 0;
            foreach (var instruction in PatchProcessor.GetOriginalInstructions(method))
                if (instruction.Calls(rate)) count++;
            if (count != expected) throw new NotSupportedException("Warp-rate decisions changed: " + method.DeclaringType.Name + "." + method.Name);
        }

        public static float IntegrationRate()
        {
            return PhysicsWarpPrecision.IsActive ? 1f : TimeWarp.CurrentRate;
        }

        public static IEnumerable<CodeInstruction> NormalizeRate(IEnumerable<CodeInstruction> source)
        {
            int changed = 0;
            var rate = AccessTools.PropertyGetter(typeof(TimeWarp), "CurrentRate");
            var replacement = AccessTools.Method(typeof(Bootstrap), "IntegrationRate");
            foreach (var instruction in source)
            {
                if (instruction.Calls(rate))
                {
                    instruction.opcode = OpCodes.Call;
                    instruction.operand = replacement;
                    changed++;
                }
                yield return instruction;
            }
            if (changed == 0) throw new NotSupportedException("Expected physical-warp rate decisions");
        }

        public static IEnumerable<CodeInstruction> MoveAutopilot(IEnumerable<CodeInstruction> source)
        {
            int changed = 0;
            var update = AccessTools.DeclaredMethod(typeof(VesselAutopilot), "Update");
            foreach (var instruction in source)
            {
                if (instruction.Calls(update))
                {
                    instruction.opcode = OpCodes.Call;
                    instruction.operand = AccessTools.Method(typeof(Bootstrap), "UpdateFromRender");
                    changed++;
                }
                yield return instruction;
            }
            if (changed != 1) throw new NotSupportedException("Expected one render-frame autopilot update");
        }

        public static void UpdateFromRender(VesselAutopilot autopilot)
        {
            if (!PhysicsWarpPrecision.IsAccelerated) autopilot.Update();
        }

        public static void BeforeInput(Vessel __instance)
        {
            if (PhysicsWarpPrecision.IsAccelerated && Time.inFixedTimeStep
                && __instance.loaded && !__instance.packed && __instance.IsControllable)
                __instance.Autopilot.Update();
        }

        public static bool BeforeWarp(int rate_index, bool instant, bool postScreenMessage)
        {
            return !PhysicsWarpPrecision.DeferWarp(rate_index, instant, postScreenMessage);
        }

        public static bool BeforeThermalFrame()
        {
            return !PhysicsWarpPrecision.IsActive || Time.inFixedTimeStep;
        }

        public static void UpdateThermalInputs(FlightIntegrator integrator)
        {
            if (PhysicsWarpPrecision.IsActive) integrator.Update();
        }

        public static IEnumerable<CodeInstruction> MoveThermalInputs(IEnumerable<CodeInstruction> source)
        {
            var thermal = AccessTools.DeclaredMethod(typeof(FlightIntegrator), "UpdateThermodynamics");
            int changed = 0;
            foreach (var instruction in source)
            {
                if (instruction.Calls(thermal))
                {
                    var duplicate = new CodeInstruction(OpCodes.Dup);
                    duplicate.labels.AddRange(instruction.labels); instruction.labels.Clear();
                    duplicate.blocks.AddRange(instruction.blocks); instruction.blocks.Clear();
                    yield return duplicate;
                    yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(Bootstrap), "UpdateThermalInputs"));
                    changed++;
                }
                yield return instruction;
            }
            if (changed != 1) throw new NotSupportedException("Expected one thermal integration call per physics tick");
        }

    }
}
