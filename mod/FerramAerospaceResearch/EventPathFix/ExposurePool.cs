using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeFarEventPath
{
    internal static class ExposurePool
    {
        const string Id = "KerbalOverEngineering.FAR.ExposurePoolReset";
        static MethodInfo stockReset;
        static Action<object> reset;
        static Func<object, List<HashSet<Renderer>>> slots;
        static Func<object, int> objectCount;
        public static long Resets, LegacyFallbacks, ReleasedSlots;

        internal static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("FAR.ExposurePoolReset")) return;
            var harmony = new Harmony(Id);
            try
            {
                var renderer = AccessTools.TypeByName("FerramAerospaceResearch.Geometry.Exposure.Renderer`1").MakeGenericType(typeof(Part));
                Prepare(renderer);
                if (!NeedsFix(PatchProcessor.GetOriginalInstructions(stockReset)))
                {
                    Debug.Log("[KOE FAR Pool] Upstream already clears renderer slots; no patch installed");
                    return;
                }
                var exposure = AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.VehicleExposure");
                var caller = AccessTools.DeclaredMethod(exposure, "ResetParts").MakeGenericMethod(typeof(List<Part>));
                var existing = Harmony.GetPatchInfo(caller);
                if (existing != null)
                    foreach (string owner in existing.Owners)
                        if (owner != "KerbalOverEngineering.FAR.RendererInitDispatch") throw new NotSupportedException("FAR exposure rebuild already patched by " + owner);
                harmony.Patch(caller, null, null, new HarmonyMethod(typeof(ExposurePool), "Rewrite"));
                Application.quitting += Report;
                Debug.Log("[KOE FAR Pool] Reset slot ownership fix enabled");
            }
            catch (Exception error)
            {
                harmony.UnpatchAll(Id);
                Debug.LogError("[KOE FAR Pool] Disabled: " + error.Message);
            }
        }

        internal static void Prepare(Type renderer)
        {
            stockReset = AccessTools.DeclaredMethod(renderer, "Reset");
            var slotField = AccessTools.Field(renderer, "renderers");
            var objectField = AccessTools.Field(renderer, "objects");
            if (stockReset == null || stockReset.ReturnType != typeof(void) || stockReset.GetParameters().Length != 0
                || slotField == null || slotField.FieldType != typeof(List<HashSet<Renderer>>) || objectField == null
                || Harmony.GetPatchInfo(stockReset) != null) throw new NotSupportedException("Unsupported FAR exposure reset layout");
            var method = new DynamicMethod("KOE_FarOriginalReset", typeof(void), new[] { typeof(object) }, typeof(ExposurePool), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Castclass, renderer); il.Emit(OpCodes.Callvirt, stockReset); il.Emit(OpCodes.Ret);
            reset = (Action<object>)method.CreateDelegate(typeof(Action<object>));
            method = new DynamicMethod("KOE_FarRendererSlots", slotField.FieldType, new[] { typeof(object) }, typeof(ExposurePool), true);
            il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Castclass, renderer); il.Emit(OpCodes.Ldfld, slotField); il.Emit(OpCodes.Ret);
            slots = (Func<object, List<HashSet<Renderer>>>)method.CreateDelegate(typeof(Func<object, List<HashSet<Renderer>>>));
            method = new DynamicMethod("KOE_FarObjectCount", typeof(int), new[] { typeof(object) }, typeof(ExposurePool), true);
            il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Castclass, renderer); il.Emit(OpCodes.Ldfld, objectField);
            il.Emit(OpCodes.Callvirt, AccessTools.PropertyGetter(objectField.FieldType, "Count")); il.Emit(OpCodes.Ret);
            objectCount = (Func<object, int>)method.CreateDelegate(typeof(Func<object, int>));
        }

        internal static bool NeedsFix(IEnumerable<CodeInstruction> code)
        {
            bool releases = false, clearsObjects = false;
            foreach (var instruction in code)
            {
                var method = instruction.operand as MethodInfo;
                if (method == null) continue;
                if (method.DeclaringType == typeof(List<HashSet<Renderer>>) && method.Name == "Clear") return false;
                if (method.Name == "Release" && method.GetParameters().Length == 1 && method.GetParameters()[0].ParameterType == typeof(HashSet<Renderer>)) releases = true;
                if (method.Name == "Clear" && method.DeclaringType.IsGenericType && method.DeclaringType.GetGenericTypeDefinition() == typeof(Dictionary<,>)) clearsObjects = true;
            }
            if (!releases || !clearsObjects) throw new NotSupportedException("FAR reset no longer has the reviewed release/object-clear sequence");
            return true;
        }

        public static void Reset(object renderer)
        {
            var list = slots(renderer);
            int count = list.Count;
            bool clean = count == objectCount(renderer);
            reset(renderer);
            if (!clean)
            {
                // AI-generated comment: Do not repair an instance whose pool ownership was already corrupted before this hook became active.
                LegacyFallbacks++;
                return;
            }
            list.Clear();
            Resets++; ReleasedSlots += count;
        }

        public static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> code)
        {
            int changed = 0;
            foreach (var instruction in code)
            {
                var copy = new CodeInstruction(instruction);
                if (copy.Calls(stockReset))
                {
                    copy.opcode = OpCodes.Call;
                    copy.operand = AccessTools.Method(typeof(ExposurePool), "Reset");
                    changed++;
                }
                yield return copy;
            }
            if (changed != 1) throw new NotSupportedException("FAR exposure rebuild no longer calls one renderer reset");
        }

        static void Report()
        {
            Debug.Log("[KOE FAR Pool] resets=" + Resets + "; releasedSlots=" + ReleasedSlots + "; legacyFallbacks=" + LegacyFallbacks);
        }
    }
}
