using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeCollisionStateFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.KSPCF.CollisionState";
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("KSPCommunityFixes.CollisionStateDedup"))
            {
                Destroy(this);
                return;
            }

            var harmony = new Harmony(Id);
            try
            {
                var type = AccessTools.TypeByName("KSPCommunityFixes.Performance.CollisionManagerFastUpdate");
                if (type == null)
                    throw new NotSupportedException("KSPCF collision optimizer not installed");
                var method = AccessTools.DeclaredMethod(type, "UpdatePartCollisionIgnoresFast");
                if (method == null || method.ReturnType != typeof(void) || !method.IsStatic || method.GetParameters().Length != 1)
                    throw new NotSupportedException("Cannot find the supported KSPCF collision-update method.");
                if (Harmony.GetPatchInfo(method) != null)
                    throw new NotSupportedException("Collision optimizer already patched");
                harmony.Patch(method, null, null, new HarmonyMethod(typeof(Bootstrap), "Transpile"));
                Debug.Log("[KOE CollisionState] Applied; actual pair state read on every refresh; no persistent collision-state cache");
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(Id);
                Debug.LogError("[KOE CollisionState] Disabled: " + ex.Message);
            }

            Destroy(this);
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>(source);
            var setter = AccessTools.Method(typeof(Physics), "IgnoreCollision", new[] { typeof(Collider), typeof(Collider), typeof(bool) });
            int edits = 0;
            foreach (var instruction in code)
            {
                if (instruction.Calls(setter))
                {
                    var replacement = new CodeInstruction(instruction);
                    replacement.opcode = OpCodes.Call;
                    replacement.operand = AccessTools.Method(typeof(Bootstrap), "EnsureState");
                    edits++;
                    yield return replacement;
                }
                else
                    yield return instruction;
            }

            if (edits != 1)
                throw new NotSupportedException("Cannot find the expected Physics.IgnoreCollision call in the KSPCF update.");
        }

        public static void EnsureState(Collider a, Collider b, bool ignored)
        {
            // AI-generated comment: Query native state, rather than trusting a stale pair cache. Changes
            // made by other mods and newly created colliders remain observable.
            if (Physics.GetIgnoreCollision(a, b) != ignored)
                Physics.IgnoreCollision(a, b, ignored);
        }
    }
}
