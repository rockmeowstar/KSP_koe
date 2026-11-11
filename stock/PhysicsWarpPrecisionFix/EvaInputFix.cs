using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using HarmonyLib;
using KerbalOverEngineering.Options;
using UnityEngine;

namespace KoePhysicsWarpPrecision
{
    public sealed class FramePressGate
    {
        sealed class State
        {
            public int Frame = int.MinValue;
            public readonly HashSet<object> Pressed = new HashSet<object>();
        }
        readonly ConditionalWeakTable<object, State> states = new ConditionalWeakTable<object, State>();
        static readonly ConditionalWeakTable<object, State>.CreateValueCallback create = Create;
        static State Create(object owner) { return new State(); }
        public bool Consume(object owner, object binding, int frame)
        {
            var state = states.GetValue(owner, create);
            if (state.Frame != frame) { state.Frame = frame; state.Pressed.Clear(); }
            return state.Pressed.Add(binding);
        }
    }

    public static class EvaInputFix
    {
        static readonly FramePressGate presses = new FramePressGate();
        public static MethodInfo Target;
        public static void Prepare()
        {
            Target = AccessTools.DeclaredMethod(typeof(KerbalEVA), "HandleMovementInput", Type.EmptyTypes);
            if (Target == null || Target.IsStatic) throw new NotSupportedException("EVA movement input changed");
            foreach (var instruction in Rewrite(PatchProcessor.GetOriginalInstructions(Target))) { }
        }
        public static bool FirstPress(KeyBinding binding, bool ignoreInputLock, KerbalEVA owner)
        {
            if (!binding.GetKeyDown(ignoreInputLock)) return false;
            return !PhysicsWarpPrecision.IsActive || !Time.inFixedTimeStep
                || presses.Consume(owner, binding, Time.frameCount);
        }
        public static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> source)
        {
            var down = AccessTools.DeclaredMethod(typeof(KeyBinding), "GetKeyDown", new[] {typeof(bool)});
            int count = 0;
            foreach (var instruction in source)
            {
                if (instruction.Calls(down))
                {
                    var owner = new CodeInstruction(OpCodes.Ldarg_0);
                    owner.labels.AddRange(instruction.labels); instruction.labels.Clear();
                    owner.blocks.AddRange(instruction.blocks); instruction.blocks.Clear();
                    yield return owner;
                    instruction.opcode = OpCodes.Call;
                    instruction.operand = AccessTools.Method(typeof(EvaInputFix), "FirstPress");
                    count++;
                }
                yield return instruction;
            }
            if (count != 1) throw new NotSupportedException("Expected one EVA movement-mode edge input");
        }
    }
}
