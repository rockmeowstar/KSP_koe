using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using HarmonyLib;

namespace KoeFreeIvaRefresh
{
    public static class ForeignRefresh
    {
        sealed class Dirty
        {
            internal bool ForeignOnly;
            internal Vessel Active;
        }

        static readonly ConditionalWeakTable<object, Dirty> states = new ConditionalWeakTable<object, Dirty>();
        static readonly ConditionalWeakTable<object, Dirty>.CreateValueCallback createState = owner => new Dirty();
        static readonly List<Part> empty = new List<Part>();
        [ThreadStatic]
        static bool skipActiveCreation;
        static Dirty State(object instance)
        {
            return states.GetValue(instance, createState);
        }

        public static void Install(Harmony harmony)
        {
            var type = AccessTools.TypeByName("FreeIva.FreeIva");
            if (type == null || type.Assembly.ManifestModule.ModuleVersionId != new Guid("44f85c42-e554-4c92-842c-629cde208456"))
                throw new NotSupportedException("Cannot find the supported FreeIVA interior-refresh methods.");
            foreach (string name in new[]
            {
                "OnVesselPartCountChanged",
                "OnSameVesselDockingChange",
                "LateUpdate",
                "EnableInternals"
            }

            )
                if (AccessTools.DeclaredMethod(type, name) == null || Harmony.GetPatchInfo(AccessTools.DeclaredMethod(type, name)) != null)
                    throw new NotSupportedException("Missing/already patched FreeIVA " + name);
            if (AccessTools.Field(type, "m_internalVisibilityDirty") == null)
                throw new NotSupportedException("Cannot find the FreeIVA fields that track interior changes.");
            harmony.Patch(AccessTools.DeclaredMethod(type, "OnVesselPartCountChanged"), new HarmonyMethod(typeof(ForeignRefresh), "PartCount"));
            harmony.Patch(AccessTools.DeclaredMethod(type, "OnSameVesselDockingChange"), new HarmonyMethod(typeof(ForeignRefresh), "Docking"));
            harmony.Patch(
                AccessTools.DeclaredMethod(type, "LateUpdate"),
                new HarmonyMethod(typeof(ForeignRefresh), "Begin"),
                null,
                null,
                new HarmonyMethod(typeof(ForeignRefresh), "End")
            );
            harmony.Patch(AccessTools.DeclaredMethod(type, "EnableInternals"), null, null, new HarmonyMethod(typeof(ForeignRefresh), "CreationParts"));
        }

        static void PartCount(object __instance, Vessel __0, bool ___m_internalVisibilityDirty)
        {
            var state = State(__instance);
            var active = FlightGlobals.ActiveVessel;
            if (!___m_internalVisibilityDirty)
            {
                state.ForeignOnly = !ReferenceEquals(__0, active);
                state.Active = active;
            }
            else if (ReferenceEquals(__0, active) || !ReferenceEquals(state.Active, active))
                state.ForeignOnly = false;
        }

        static void Docking(object __instance)
        {
            State(__instance).ForeignOnly = false;
        }

        static void Begin(object __instance, bool ___m_internalVisibilityDirty, out bool __state)
        {
            __state = skipActiveCreation;
            var state = State(__instance);
            skipActiveCreation = ___m_internalVisibilityDirty
                && state.ForeignOnly
                && ReferenceEquals(state.Active, FlightGlobals.ActiveVessel);
        }

        static Exception End(object __instance, bool __state, Exception __exception)
        {
            skipActiveCreation = __state;
            var state = State(__instance);
            state.ForeignOnly = false;
            state.Active = null;
            return __exception;
        }

        public static List<Part> CreationTargets(List<Part> parts)
        {
            return skipActiveCreation ? empty : parts;
        }

        public static IEnumerable<CodeInstruction> CreationParts(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>(source);
            int count = 0;
            var enumerate = AccessTools.Method(typeof(List<Part>), "GetEnumerator");
            foreach (var instruction in code)
            {
                if (instruction.Calls(enumerate))
                {
                    count++;
                    // AI-generated comment: Preserve the probe-core branch and the final global foreign
                    // cleanup/hatch/mask refresh. Skip only active-vessel creation
                    // when all pending part-count events concern other vessels.
                    if (count == 2)
                    {
                        if (instruction.labels.Count != 0 || instruction.blocks.Count != 0)
                            throw new NotSupportedException("The FreeIVA interior-creation loop has a different branch structure.");
                        yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(ForeignRefresh), "CreationTargets"));
                    }
                }

                yield return instruction;
            }

            if (count != 2)
                throw new NotSupportedException("Cannot find the vessel-part enumeration in FreeIVA EnableInternals.");
        }
    }
}
