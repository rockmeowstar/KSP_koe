using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Threading;
using HarmonyLib;
using KoePhysicsWarpPrecision;
using Patches = KoeFarPhysicsPrecision.Patches;

namespace KerbalOverEngineering.Options
{
    public static class Settings { public static bool Enabled(string key) { return true; } }
    public static class PhysicsWarpPrecision
    {
        public static bool PatchesReady, IsActive, IsAccelerated;
        public static bool DeferWarp(int rate, bool instant, bool message) { return false; }
    }
}

static class PhysicsPrecisionPatchTests
{
    static string game;
    static int checks;
    static void Check(bool value, string message) { checks++; if (!value) throw new Exception(message); }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        var loaded = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == name);
        if (loaded != null) return loaded;
        foreach (string path in new[] { Path.Combine(game, "KSP_x64_Data/Managed", name + ".dll"),
            Path.Combine(game, "GameData/000_Harmony", name + ".dll"),
            Path.Combine(game, "GameData/FerramAerospaceResearch/Plugins", name + ".dll") })
            if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
        foreach (string path in Directory.GetFiles(Path.Combine(game, "GameData"), "*" + name + ".dll", SearchOption.AllDirectories))
            if (AssemblyName.GetAssemblyName(path).Name == name) return Assembly.Load(File.ReadAllBytes(path));
        return null;
    }
    static void Main(string[] args)
    {
        game = args[0]; AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        Run();
    }
    static void Rewrite(MethodInfo target, Func<IEnumerable<CodeInstruction>, IEnumerable<CodeInstruction>> rewrite,
        MethodInfo originalCall, MethodInfo replacement, int expected)
    {
        var original = PatchProcessor.GetOriginalInstructions(target).ToList();
        var copy = original.Select(i => new CodeInstruction(i)).ToList();
        var result = rewrite(copy).ToList();
        Check(original.Count == result.Count, "Unexpected instruction count: " + target.Name);
        Check(original.Count(i => i.Calls(originalCall)) == expected, "Installed call count changed: " + target.Name);
        Check(result.Count(i => i.Calls(replacement)) == expected, "Replacement count wrong: " + target.Name);
        for (int i = 0; i < original.Count; i++)
        {
            Check(original[i].labels.SequenceEqual(result[i].labels), "Branch labels changed");
            Check(original[i].blocks.SequenceEqual(result[i].blocks), "Exception regions changed");
            if (!original[i].Calls(originalCall))
                Check(original[i].opcode == result[i].opcode && Equals(original[i].operand, result[i].operand), "Unrelated operation changed");
        }
        bool rejected = false;
        try { rewrite(new CodeInstruction[0]).ToList(); } catch (NotSupportedException) { rejected = true; }
        Check(rejected, "Missing call layout accepted");
    }

    sealed class Pool
    {
        readonly Queue<Action> main = new Queue<Action>();
        public int Queued, Pumped, WorkerThread;
        public void Queue(object ignored, Action action)
        {
            Queued++;
            ThreadPool.QueueUserWorkItem(delegate { WorkerThread = Thread.CurrentThread.ManagedThreadId; action(); });
        }
        public void MainThread(Action action)
        {
            using (var done = new ManualResetEvent(false))
            {
                lock (main) main.Enqueue(delegate { action(); done.Set(); });
                if (!done.WaitOne(5000)) throw new Exception("Test watchdog: main-thread action was not pumped");
            }
        }
        public void Pump(object ignored)
        {
            for (;;)
            {
                Action action;
                lock (main) { if (main.Count == 0) return; action = main.Dequeue(); }
                Pumped++; action();
            }
        }
    }

    static void Run()
    {
        CheckStepContracts();
        EvaInputFix.Prepare();
        var eva = PatchProcessor.GetOriginalInstructions(EvaInputFix.Target).ToList();
        var evaResult = EvaInputFix.Rewrite(eva.Select(i => new CodeInstruction(i))).ToList();
        Check(evaResult.Count == eva.Count+1, "EVA edge patch must add only its owner argument");
        Check(evaResult.Count(i => i.Calls(AccessTools.Method(typeof(EvaInputFix), "FirstPress"))) == 1, "EVA edge gate missing");
        bool noEva = false;
        try { EvaInputFix.Rewrite(new CodeInstruction[0]).ToList(); } catch (NotSupportedException) { noEva = true; }
        Check(noEva, "Changed EVA input layout accepted");
        var gate = new FramePressGate();
        var owner1 = new object(); var owner2 = new object(); var key1 = new object(); var key2 = new object();
        foreach (int steps in new[] { 1,10,20,40 })
        {
            int accepted = 0;
            for (int step = 0; step < steps; step++) if (gate.Consume(owner1,key1,steps)) accepted++;
            Check(accepted == 1, "A movement-mode press repeated during catch-up steps");
            Check(gate.Consume(owner2,key1,steps), "One kerbal consumed another kerbal's press");
            Check(gate.Consume(owner1,key2,steps), "Distinct input bindings were merged");
        }
        Check(gate.Consume(owner1,key1,41), "New frame press was lost");
        var rate = AccessTools.PropertyGetter(typeof(TimeWarp), "CurrentRate");
        var normalized = AccessTools.Method(typeof(Bootstrap), "IntegrationRate");
        var sas = typeof(VesselAutopilot).GetNestedType("VesselSAS", BindingFlags.Public | BindingFlags.NonPublic);
        Rewrite(AccessTools.DeclaredMethod(sas, "AutoTuneScalar"), Bootstrap.NormalizeRate, rate, normalized, 7);
        Rewrite(AccessTools.DeclaredMethod(typeof(FlightIntegrator), "UpdateThermodynamics"), Bootstrap.NormalizeRate, rate, normalized, 2);
        Rewrite(AccessTools.DeclaredMethod(typeof(Vessel), "Update"), Bootstrap.MoveAutopilot,
            AccessTools.Method(typeof(VesselAutopilot), "Update"), AccessTools.Method(typeof(Bootstrap), "UpdateFromRender"), 1);
        KerbalOverEngineering.Options.PhysicsWarpPrecision.IsActive = true;
        var tick = PatchProcessor.GetOriginalInstructions(AccessTools.DeclaredMethod(typeof(FlightIntegrator), "FixedUpdate")).ToList();
        var result = Bootstrap.MoveThermalInputs(tick.Select(i => new CodeInstruction(i))).ToList();
        Check(result.Count == tick.Count+2, "Thermal inputs must add just one receiver copy and call");
        int offset = result.FindIndex(i => i.Calls(AccessTools.Method(typeof(Bootstrap), "UpdateThermalInputs")));
        Check(offset > 0 && result[offset-1].opcode == OpCodes.Dup, "Thermal receiver not preserved");
        Check(result[offset+1].Calls(AccessTools.Method(typeof(FlightIntegrator), "UpdateThermodynamics")), "Thermal inputs must precede integration");
        KerbalOverEngineering.Options.PhysicsWarpPrecision.IsAccelerated = true;
        Check(Bootstrap.IntegrationRate() == 1, "SAS and thermal rate must follow the base step");
        Bootstrap.UpdateFromRender(null);
        Check(true, "Active precision must skip render-frame autopilot updates");
        KerbalOverEngineering.Options.PhysicsWarpPrecision.IsAccelerated = false;
        bool stockUpdate = false;
        try { Bootstrap.UpdateFromRender(null); } catch (NullReferenceException) { stockUpdate = true; }
        Check(stockUpdate, "1x must retain the stock autopilot update path");

        Assembly.Load(File.ReadAllBytes(Path.Combine(game, "GameData/FerramAerospaceResearch/Plugins/FerramAerospaceResearch.dll")));
        Patches.Prepare();
        var aero = AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.VehicleAerodynamics");
        var original = PatchProcessor.GetOriginalInstructions(AccessTools.Method(aero, "TryVoxelUpdate")).ToList();
        var rewritten = Patches.SubmitAtTick(original.Select(i => new CodeInstruction(i))).ToList();
        Check(rewritten.Count == original.Count + 1, "FAR must add just the owner argument");
        Check(rewritten.Count(i => i.Calls(AccessTools.Method(typeof(Patches), "Submit"))) == 1, "FAR submission replacement missing");
        bool rejected = false;
        try { Patches.SubmitAtTick(new CodeInstruction[0]).ToList(); } catch (NotSupportedException) { rejected = true; }
        Check(rejected, "Changed FAR submission layout accepted");

        var pool = new Pool();
        AccessTools.Field(typeof(Patches), "enqueue").SetValue(null, new Action<object, Action>(pool.Queue));
        AccessTools.Field(typeof(Patches), "pump").SetValue(null, new Action<object>(pool.Pump));
        AccessTools.Field(typeof(Patches), "geometryReady").SetValue(null, new Func<object, bool>(owner => true));
        KerbalOverEngineering.Options.PhysicsWarpPrecision.IsAccelerated = true;
        int mainThread = Thread.CurrentThread.ManagedThreadId;
        for (int i = 0; i < 12; i++)
        {
            var owner = new object();
            bool published = false;
            lock (owner)
                Patches.Submit(pool, delegate
                {
                    lock (owner) pool.MainThread(delegate
                    {
                        Check(Thread.CurrentThread.ManagedThreadId == mainThread, "FAR callback left main thread");
                        published = true;
                    });
                }, owner);
            Patches.AfterSubmission(owner);
            Check(published, "Physics resumed before geometry publication");
            Patches.AfterSubmission(owner);
        }
        Check(pool.WorkerThread != mainThread, "Voxel computation must retain its worker thread");
        Check(pool.Pumped == 12, "Main-thread tasks were dropped or duplicated");
        AccessTools.Field(typeof(Patches), "geometryReady").SetValue(null, new Func<object, bool>(owner => false));
        var waiting = new object();
        using (var finished = new ManualResetEvent(false))
        {
            Patches.Submit(pool, delegate { finished.Set(); }, waiting);
            Patches.AfterSubmission(waiting);
            Check(finished.WaitOne(5000), "Unready geometry was not queued asynchronously");
        }
        AccessTools.Field(typeof(Patches), "geometryReady").SetValue(null, new Func<object, bool>(owner => true));
        var failed = new object();
        Patches.Submit(pool, delegate { throw new InvalidOperationException("worker failure"); }, failed);
        bool propagated = false;
        try { Patches.AfterSubmission(failed); } catch (InvalidOperationException e) { propagated = e.InnerException != null; }
        Check(propagated, "Worker error hidden");
        Patches.AfterSubmission(failed);

        KerbalOverEngineering.Options.PhysicsWarpPrecision.IsActive = false;
        using (var done = new ManualResetEvent(false))
        {
            var owner = new object();
            Patches.Submit(pool, delegate { done.Set(); }, owner);
            Patches.AfterSubmission(owner);
            Check(done.WaitOne(5000), "Disabled precision changed FAR submission");
        }
        Console.WriteLine("Physics precision patches: " + checks + " offline checks passed. No gameplay validation.");
    }

    static void RequireStepCalls(Type type, string method, MethodInfo step, int count)
    {
        var target = AccessTools.DeclaredMethod(type, method);
        Check(target != null, "Missing component method: " + type.Name + "." + method);
        Check(PatchProcessor.GetOriginalInstructions(target).Count(i => i.Calls(step)) == count,
            "Component timestep contract changed: " + type.Name + "." + method);
    }
    static void CheckStepContracts()
    {
        var step = AccessTools.PropertyGetter(typeof(TimeWarp), "fixedDeltaTime");
        var unityStep = AccessTools.PropertyGetter(typeof(UnityEngine.Time), "fixedDeltaTime");
        RequireStepCalls(typeof(ModuleRCS), "CalculateThrust", step, 1);
        RequireStepCalls(typeof(ModuleReactionWheel), "ActiveUpdate", step, 1);
        RequireStepCalls(typeof(ModuleWheels.ModuleWheelDamage), "FixedUpdate", step, 3);
        RequireStepCalls(typeof(ModuleWheels.ModuleWheelSuspension), "UpdateAutoBoost", unityStep, 2);
        RequireStepCalls(typeof(KerbalEVA), "UpdateOrientationPID", unityStep, 2);
        RequireStepCalls(typeof(KerbalEVA), "UpdatePackAngular", unityStep, 1);
        RequireStepCalls(typeof(KerbalEVA), "UpdatePackLinear", unityStep, 1);
        var evaTick = PatchProcessor.GetOriginalInstructions(AccessTools.DeclaredMethod(typeof(KerbalEVA), "FixedUpdate"));
        Check(evaTick.Count(i => i.Calls(AccessTools.DeclaredMethod(typeof(KerbalEVA), "HandleMovementInput"))) == 1,
            "EVA inputs must be sampled from the physical tick");
        RequireStepCalls(typeof(ModuleWheelBase), "FixedUpdate", AccessTools.PropertyGetter(typeof(TimeWarp), "deltaTime"), 1);
    }
}
