using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KoePlanetLaunchReadiness;
using UnityEngine;

static class PlanetLaunchReadinessTests
{
    static string game;
    static int checks;
    static void Check(bool value, string message) { checks++; if (!value) throw new Exception(message); }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        var loaded = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == name);
        if (loaded != null) return loaded;
        foreach (string directory in new[] { "KSP_x64_Data/Managed", "GameData/000_Harmony" })
        {
            string path = Path.Combine(game, directory, name + ".dll");
            if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
        }
        return null;
    }
    static void Main(string[] args) { game = args[0]; AppDomain.CurrentDomain.AssemblyResolve += Resolve; Run(); }
    static void Reject(IEnumerable<CodeInstruction> source, string message)
    {
        bool rejected = false; try { Bootstrap.Rewrite(source).ToList(); } catch (NotSupportedException) { rejected = true; }
        Check(rejected, message);
    }
    static void Run()
    {
        var iterator = typeof(PSystemSetup).GetNestedTypes(BindingFlags.Public | BindingFlags.NonPublic).Single(t => t.Name.StartsWith("<SetupLaunchSites>", StringComparison.Ordinal));
        var original = PatchProcessor.GetOriginalInstructions(AccessTools.DeclaredMethod(iterator, "MoveNext")).ToList();
        var rewritten = Bootstrap.Rewrite(original).ToList();
        var constructor = AccessTools.Constructor(typeof(WaitForSeconds), new[] { typeof(float) });
        var helper = AccessTools.DeclaredMethod(typeof(Bootstrap), "Wait");
        int changed = 0, j = 0;
        foreach (var instruction in original)
        {
            var result = rewritten[j++];
            Check(instruction.labels.SequenceEqual(result.labels), "Keep all branch destinations");
            Check(instruction.blocks.SequenceEqual(result.blocks), "Keep exception boundaries");
            if (instruction.opcode == OpCodes.Newobj && Equals(instruction.operand, constructor))
            {
                changed++;
                Check(result.opcode == OpCodes.Ldarg_0, "Supply the current iterator");
                var call = rewritten[j++];
                Check(call.Calls(helper) && call.labels.Count == 0 && call.blocks.Count == 0, "Only timer construction calls the helper");
                Check(helper.ReturnType == typeof(object), "Yield field receives an object");
            }
            else Check(result.opcode == instruction.opcode && Equals(result.operand, instruction.operand), "Keep every non-timer instruction");
        }
        Check(changed == 2 && j == rewritten.Count, "Exactly two additional iterator arguments");
        var traced = Bootstrap.Trace(rewritten).ToList();
        int traceOffset = 0;
        var lastStep = AccessTools.Field(typeof(Bootstrap), "LastStep");
        foreach (var instruction in rewritten)
        {
            var marker = traced[traceOffset];
            bool marked = traceOffset + 1 < traced.Count && marker.opcode == OpCodes.Ldc_I4 && traced[traceOffset + 1].opcode == OpCodes.Stsfld && Equals(traced[traceOffset + 1].operand, lastStep);
            if (marked) traceOffset += 2;
            var result = traced[traceOffset++];
            Check(result.opcode == instruction.opcode && Equals(result.operand, instruction.operand), "Tracing preserves every rewritten instruction");
            Check(instruction.labels.SequenceEqual(marked ? marker.labels : result.labels), "Tracing keeps original branch destinations on its marker");
            Check(instruction.blocks.SequenceEqual(marked ? marker.blocks : result.blocks), "Tracing keeps original exception boundaries");
        }
        Check(traceOffset == traced.Count, "Trace only inserts stack-neutral scalar markers");
        for (int bits = 0; bits < 16; bits++)
            Check(Bootstrap.CanShorten((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0) == (bits == 7), "Require completed initial geometry and a live initialized sphere; exclude collider raycasts");
        Reject(new CodeInstruction[0], "Reject missing timers");
        var changedCode = original.Select(i => new CodeInstruction(i)).ToList();
        int timer = changedCode.FindIndex(i => i.opcode == OpCodes.Newobj && Equals(i.operand, constructor));
        changedCode[timer - 1].opcode = OpCodes.Ldc_I4_2;
        Reject(changedCode, "Reject another timer argument shape");
        changedCode = original.Select(i => new CodeInstruction(i)).ToList();
        timer = changedCode.FindIndex(i => i.opcode == OpCodes.Newobj && Equals(i.operand, constructor));
        changedCode[timer + 1].opcode = OpCodes.Pop;
        Reject(changedCode, "Reject a timer not directly yielded by the iterator");
        Console.WriteLine("Launch readiness: " + checks + " IL/predicate checks passed; runtime readiness and content parity pending.");
    }
}
