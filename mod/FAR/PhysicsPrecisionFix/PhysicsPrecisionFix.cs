using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Threading;
using HarmonyLib;
using KerbalOverEngineering.Options;

namespace KoeFarPhysicsPrecision
{
    public static class Patches
    {
        sealed class Job
        {
            public object Pool;
            public bool Completed;
            public Exception Error;
        }
        static readonly ConditionalWeakTable<object, Job> jobs = new ConditionalWeakTable<object, Job>();
        static MethodInfo voxelUpdate, queue;
        static Action<object, Action> enqueue;
        static Action<object> pump;

        public static void Prepare()
        {
            voxelUpdate = null;
            var aero = AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.VehicleAerodynamics");
            if (aero == null) return;
            var pool = AccessTools.TypeByName("FerramAerospaceResearch.FARThreading.VoxelizationThreadpool");
            if (pool == null) throw new NotSupportedException("FAR voxel threadpool unavailable");
            voxelUpdate = AccessTools.DeclaredMethod(aero, "TryVoxelUpdate");
            queue = AccessTools.DeclaredMethod(pool, "QueueVoxelization", new[] { typeof(Action) });
            var execute = AccessTools.DeclaredMethod(pool, "ExecuteMainThreadTasks", Type.EmptyTypes);
            if (voxelUpdate == null || voxelUpdate.ReturnType != typeof(bool) || queue == null || execute == null
                || queue.IsStatic || execute.IsStatic || queue.ReturnType != typeof(void) || execute.ReturnType != typeof(void))
                throw new NotSupportedException("FAR voxel publication contract changed");
            enqueue = (Action<object, Action>)Forwarder(queue, typeof(Action<object, Action>));
            pump = (Action<object>)Forwarder(execute, typeof(Action<object>));
            bool releasesLock = false;
            foreach (var instruction in PatchProcessor.GetOriginalInstructions(voxelUpdate))
                releasesLock |= instruction.Calls(AccessTools.Method(typeof(Monitor), "Exit"));
            if (!releasesLock) throw new NotSupportedException("FAR voxel submission no longer releases its monitor");
            foreach (var instruction in SubmitAtTick(PatchProcessor.GetOriginalInstructions(voxelUpdate))) { }
        }

        static Delegate Forwarder(MethodInfo method, Type signature)
        {
            var parameters = signature.GetMethod("Invoke").GetParameters();
            var types = new Type[parameters.Length];
            for (int i = 0; i < types.Length; i++) types[i] = parameters[i].ParameterType;
            var forward = new DynamicMethod("KoeFar_" + method.Name, typeof(void), types, typeof(Patches), true);
            var il = forward.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Castclass, method.DeclaringType);
            if (types.Length == 2) il.Emit(OpCodes.Ldarg_1);
            il.Emit(OpCodes.Callvirt, method); il.Emit(OpCodes.Ret);
            return forward.CreateDelegate(signature);
        }

        public static void Install(Harmony harmony)
        {
            if (voxelUpdate == null) return;
            harmony.Patch(voxelUpdate, postfix: new HarmonyMethod(typeof(Patches), "AfterSubmission"),
                transpiler: new HarmonyMethod(typeof(Patches), "SubmitAtTick"));
        }

        public static IEnumerable<CodeInstruction> SubmitAtTick(IEnumerable<CodeInstruction> source)
        {
            int changed = 0;
            foreach (var instruction in source)
            {
                if (!instruction.Calls(queue)) { yield return instruction; continue; }
                var owner = new CodeInstruction(OpCodes.Ldarg_0);
                owner.labels.AddRange(instruction.labels); instruction.labels.Clear();
                owner.blocks.AddRange(instruction.blocks); instruction.blocks.Clear();
                yield return owner;
                instruction.opcode = OpCodes.Call;
                instruction.operand = AccessTools.Method(typeof(Patches), "Submit");
                yield return instruction;
                changed++;
            }
            if (changed != 1) throw new NotSupportedException("Expected one FAR voxel submission");
        }

        public static void Submit(object pool, Action action, object owner)
        {
            if (!PhysicsWarpPrecision.IsActive) { enqueue(pool, action); return; }
            var job = new Job { Pool = pool };
            jobs.Add(owner, job);
            try
            {
                enqueue(pool, delegate
                {
                    try { action(); }
                    catch (Exception error) { job.Error = error; }
                    finally { lock (job) { job.Completed = true; Monitor.PulseAll(job); } }
                });
            }
            catch { jobs.Remove(owner); throw; }
        }

        public static void AfterSubmission(object __instance)
        {
            Job job;
            if (!jobs.TryGetValue(__instance, out job)) return;
            try
            {
                // AI-generated comment: Wait after FAR releases its aero lock. Workers can synchronously
                // request main-thread actions, so drain those actions instead of blocking on the worker alone.
                for (;;)
                {
                    pump(job.Pool);
                    lock (job)
                    {
                        if (job.Completed) break;
                        Monitor.Wait(job, 1);
                    }
                }
                if (job.Error != null) throw new InvalidOperationException("FAR voxel calculation failed", job.Error);
            }
            finally { jobs.Remove(__instance); }
        }
    }
}
