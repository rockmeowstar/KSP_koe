using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using CloudLoopFix;
using HarmonyLib;
using Patch = CloudLoopFix.Patch;

static class CloudLoopTests
{
    enum Nature { Bottom, Top }
    struct Bound { public float radius; public object layer; public Nature nature; }
    sealed class Layer { public float Key; public int Id; }
    sealed class EqualMaterial
    {
        public override bool Equals(object other) { return other is EqualMaterial; }
        public override int GetHashCode() { return 1; }
    }
    static int assertions;
    static string game;
    static void Check(bool condition, string message) { assertions++; if (!condition) throw new Exception(message); }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (string path in new[] { @"KSP_x64_Data\Managed", @"GameData\000_Harmony", @"GameData\EnvironmentalVisualEnhancements\Plugins" })
        {
            string file = Path.Combine(game, path, name);
            if (File.Exists(file)) return Assembly.Load(File.ReadAllBytes(file));
        }
        return null;
    }
    static int Main(string[] args)
    {
        try { game = args[0]; AppDomain.CurrentDomain.AssemblyResolve += Resolve; Run(); return 0; }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        CheckMaterialScopes();
        var key = Patch.MakeBoundReader<Bound>();
        var a = new object(); var b = new object();
        var source = new List<Bound> { new Bound { radius = 100, layer = a, nature = Nature.Bottom } };
        var cache = new ListMemo<Bound, BoundKey, object>(); object result;
        Check(!cache.TryGet(source, key, out result), "Cold miss");
        result = new object(); cache.Commit(source, key, result);
        object expected = result;
        for (int i = 0; i < 1000; i++) Check(cache.TryGet(source, key, out result) && ReferenceEquals(result, expected), "Same input reuses original output");
        var v = source[0]; v.radius++; source[0] = v;
        Check(!cache.TryGet(source, key, out result), "Altitude edit invalidates"); cache.Commit(source, key, expected);
        v.layer = b; source[0] = v;
        Check(!cache.TryGet(source, key, out result), "Layer identity invalidates"); cache.Commit(source, key, expected);
        v.nature = Nature.Top; source[0] = v;
        Check(!cache.TryGet(source, key, out result), "Bound nature invalidates");
        Check(!new BoundKey(0f, a, 0).Equals(new BoundKey(BitConverter.ToSingle(BitConverter.GetBytes(int.MinValue), 0), a, 0)), "Signed zero preserved bitwise");
        source.Add(new Bound()); Check(!cache.TryGet(source, key, out result), "Count invalidates");
        cache.Commit(source, key, expected); source.Clear(); cache.Commit(source, key, expected);
        Check(cache.TryGet(source, key, out result), "Empty list reusable after shrink");

        var random = new Random(88231);
        var layers = new List<Layer>();
        Func<Layer, float> getKey = x => x.Key;
        for (int run = 0; run < 500; run++)
        {
            if (run % 7 == 0) { layers.Clear(); for (int i = 0; i < random.Next(1, 80); i++) layers.Add(new Layer { Id = i }); }
            foreach (Layer layer in layers) layer.Key = random.Next(-4, 5);
            if (run % 11 == 0 && layers.Count > 1) { layers[0].Key = int.MinValue; layers[1].Key = int.MaxValue; }
            if (run % 13 == 0 && layers.Count > 2) { layers[0].Key = float.NaN; layers[1].Key = float.PositiveInfinity; layers[2].Key = float.NegativeInfinity; }
            var original = layers.OrderBy(getKey).ToList();
            var actual = LayerOrder<Layer>.Sort(layers, getKey);
            Check(actual.SequenceEqual(original), "Matches original stable LINQ order including ties");
            var again = LayerOrder<Layer>.Sort(layers, getKey);
            Check(ReferenceEquals(actual, again), "Stable input reuses sorted list");
            layers.Reverse();
            Check(LayerOrder<Layer>.Sort(layers, getKey).SequenceEqual(layers.OrderBy(getKey)), "Input order/tie edits invalidate");
        }
        layers.Clear(); layers.Add(new Layer { Key = 1 }); layers.Add(new Layer { Key = 2 });
        LayerOrder<Layer>.Sort(layers, getKey); layers[0].Key = 5;
        bool threw = false;
        try { LayerOrder<Layer>.Sort(layers, x => { if (ReferenceEquals(x, layers[1])) throw new ArgumentException(); return x.Key; }); }
        catch (ArgumentException) { threw = true; }
        Check(threw && LayerOrder<Layer>.Sort(layers, getKey).SequenceEqual(layers.OrderBy(getKey)), "Getter failure cannot retain stale order");

        Assembly atmosphere = Assembly.Load(File.ReadAllBytes(Path.Combine(game, @"GameData\EnvironmentalVisualEnhancements\Plugins\Atmosphere.dll")));
        Type renderer = atmosphere.GetType("Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer", true);
        Patch.Prepare(atmosphere);
        MethodInfo commands = AccessTools.DeclaredMethod(renderer, "HandleRenderingCommands");
        var validationMethod = new DynamicMethod("ValidateMaterialGuard", typeof(void), Type.EmptyTypes);
        var generator = validationMethod.GetILGenerator();
        var originalIL = PatchProcessor.GetOriginalInstructions(commands, generator);
        var modifiedIL = Patch.Transpile(originalIL).ToList();
        Check(originalIL.Count == modifiedIL.Count, "Instruction count retained");
        int changed = 0, draws = 0, writes = 0;
        for (int i = 0; i < originalIL.Count; i++)
        {
            Check(originalIL[i].labels.Count == modifiedIL[i].labels.Count && originalIL[i].blocks.Count == modifiedIL[i].blocks.Count, "Branch and exception metadata retained");
            if (originalIL[i].opcode != modifiedIL[i].opcode || !Equals(originalIL[i].operand, modifiedIL[i].operand)) changed++;
            var method = originalIL[i].operand as MethodInfo;
            if (method != null && (method.Name == "DrawRenderer" || method.Name.StartsWith("Set")))
            {
                Check(Equals(originalIL[i].operand, modifiedIL[i].operand), "All GPU draws and writes retained");
                if (method.Name == "DrawRenderer") draws++; else writes++;
            }
        }
        Check(changed == 2 && draws > 0 && writes > 0, "Only OrderBy and ToList replaced");
        bool rejected = false; try { Patch.Transpile(new CodeInstruction[0]).ToList(); } catch (NotSupportedException) { rejected = true; }
        Check(rejected, "Unexpected sort layout refused");

        var labelsBefore = modifiedIL.Select(c => c.labels.ToArray()).ToArray();
        var guardedIL = Patch.TranspileMaterialSetup(modifiedIL, generator).ToList();
        Check(guardedIL.Count == modifiedIL.Count + 3, "Only one identity guard added");
        int guard = guardedIL.FindIndex(c => c.operand is MethodInfo && ((MethodInfo)c.operand).Name == "ShouldSetup");
        Check(guard > 0 && guardedIL[guard + 1].opcode == OpCodes.Brfalse, "Setup guard branches on repeated material");
        var target = (Label)guardedIL[guard + 1].operand;
        int resume = guardedIL.FindIndex(c => c.labels.Contains(target));
        int conditionalWrites = guardedIL.Skip(guard + 2).Take(resume - guard - 2).Count(c =>
            c.operand is MethodInfo && ((MethodInfo)c.operand).DeclaringType.FullName == "UnityEngine.Material" &&
            ((MethodInfo)c.operand).Name.StartsWith("Set"));
        Check(conditionalWrites == 8, "Exactly eight invariant writes guarded");
        var retained = guardedIL.Where((c, i) => i < guard - 1 || i > guard + 1).ToList();
        Check(retained.Count == modifiedIL.Count, "All original instructions retained");
        for (int i = 0; i < retained.Count; i++)
            Check(retained[i].opcode == modifiedIL[i].opcode && Equals(retained[i].operand, modifiedIL[i].operand), "Original draw order and calculations retained");
        Check(modifiedIL.Select((c, i) => c.labels.SequenceEqual(labelsBefore[i])).All(equal => equal), "Transpiler does not mutate source labels");
        rejected = false;
        try { Patch.TranspileMaterialSetup(new CodeInstruction[0], validationMethod.GetILGenerator()).ToList(); }
        catch (NotSupportedException) { rejected = true; }
        Check(rejected, "Unexpected material layout refused");

        // Exercise actual private EVE value types and a real Harmony detour on the desktop CLR.
        // This method only assembles managed lists: no Unity renderer or GPU is invoked.
        MethodInfo intervals = AccessTools.DeclaredMethod(renderer, "ResolveLayerOverlapIntervals");
        Type listType = intervals.GetParameters()[0].ParameterType;
        var bounds = (IList)Activator.CreateInstance(listType);
        object instance = FormatterServices.GetUninitializedObject(renderer);
        object baseline = intervals.Invoke(instance, new object[] { bounds });
        Type hooks = typeof(IntervalHooks<,>).MakeGenericType(listType.GetGenericArguments()[0], intervals.ReturnType.GetGenericArguments()[0]);
        var harmony = new Harmony("koe.tests.cloud-loops");
        try
        {
            harmony.Patch(intervals, new HarmonyMethod(hooks.GetMethod("Prefix")), new HarmonyMethod(hooks.GetMethod("Postfix")));
            object first = intervals.Invoke(instance, new object[] { bounds });
            object second = intervals.Invoke(instance, new object[] { bounds });
            Check(((IList)baseline).Count == ((IList)first).Count && ReferenceEquals(first, second), "Actual EVE interval method detour and private generic types work");
            Type boundType = listType.GetGenericArguments()[0];
            object bound = Activator.CreateInstance(boundType);
            AccessTools.Field(boundType, "radius").SetValue(bound, 100f);
            // A null volume is legal for the interval assembler's HashSet; no Unity Object methods needed.
            bounds.Add(bound);
            object changedBounds = intervals.Invoke(instance, new object[] { bounds });
            Check(!ReferenceEquals(second, changedBounds), "Actual EVE boundary edit invalidates");
        }
        finally { harmony.Unpatch(intervals, HarmonyPatchType.All, "koe.tests.cloud-loops"); }
        Console.WriteLine("CLOUD_LOOP_MEMO_AND_ACTUAL_EVE_IL_PASS assertions=" + assertions + "; sortChangedInstructions=2; materialGuardInstructions=3; draws=" + draws + "; writeSites=" + writes + "; guardedWrites=8");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static bool ScopedCall(object material, bool fail)
    {
        bool first = MaterialSetupScope.ShouldSetup(material);
        if (fail) throw new InvalidOperationException("Expected scope failure");
        return first && !MaterialSetupScope.ShouldSetup(material);
    }

    static void CheckMaterialScopes()
    {
        var a = new EqualMaterial(); var b = new EqualMaterial();
        Check(MaterialSetupScope.ShouldSetup(a) && MaterialSetupScope.ShouldSetup(a), "Unscoped calls never suppress setup");
        MaterialSetupScope outer, inner;
        MaterialSetupScope.Begin(out outer);
        try
        {
            long applied = MaterialSetupScope.AppliedSetups, skipped = MaterialSetupScope.SkippedSetups;
            Check(MaterialSetupScope.ShouldSetup(a) && !MaterialSetupScope.ShouldSetup(a), "One setup per material per invocation");
            Check(MaterialSetupScope.AppliedSetups == applied + 1 && MaterialSetupScope.SkippedSetups == skipped + 1, "Material setup and eight avoided-write counters exact");
            Check(MaterialSetupScope.ShouldSetup(b), "Equal but distinct materials stay independent");
            MaterialSetupScope.Begin(out inner);
            try { Check(MaterialSetupScope.ShouldSetup(a) && !MaterialSetupScope.ShouldSetup(a), "Nested camera starts fresh"); }
            finally { MaterialSetupScope.End(null, inner); }
            Check(!MaterialSetupScope.ShouldSetup(a), "Nested return restores outer invocation");
        }
        finally { MaterialSetupScope.End(null, outer); }
        MaterialSetupScope.Begin(out outer);
        try { Check(MaterialSetupScope.ShouldSetup(a), "Next invocation refreshes all material properties"); }
        finally { MaterialSetupScope.End(null, outer); }
        var harmony = new Harmony("koe.tests.cloud-material-scopes");
        var call = AccessTools.DeclaredMethod(typeof(CloudLoopTests), "ScopedCall");
        try
        {
            harmony.Patch(call, new HarmonyMethod(typeof(MaterialSetupScope), "Begin"), null, null,
                new HarmonyMethod(typeof(MaterialSetupScope), "End"));
            Check(ScopedCall(a, false) && ScopedCall(a, false), "Harmony state starts and ends each invocation");
            bool threw = false;
            try { ScopedCall(a, true); } catch (InvalidOperationException) { threw = true; }
            Check(threw && ScopedCall(a, false), "Harmony finalizer preserves exception and releases failed invocation");
            Check(MaterialSetupScope.ShouldSetup(a) && MaterialSetupScope.ShouldSetup(a), "No scope leaks after detour");
        }
        finally { harmony.Unpatch(call, HarmonyPatchType.All, harmony.Id); }
    }
}
