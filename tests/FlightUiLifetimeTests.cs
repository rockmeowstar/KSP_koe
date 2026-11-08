using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using KSP.UI;
using KoeFlightUiLifetime;
using Object = UnityEngine.Object;

static class FlightUiLifetimeTests
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
            Path.Combine(game, "GameData/KerbalOverEngineering/Plugins", name + ".dll") })
            if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
        return null;
    }
    static void Main(string[] args)
    {
        game = args[0]; AppDomain.CurrentDomain.AssemblyResolve += Resolve; Run();
    }
    static void Run()
    {
        var target = AccessTools.DeclaredMethod(typeof(UIMasterController), "OnSceneChange");
        var original = PatchProcessor.GetOriginalInstructions(target).ToList();
        var rewritten = Bootstrap.Rewrite(original).ToList();
        var deferred = AccessTools.DeclaredMethod(typeof(Object), "Destroy", new[] { typeof(Object) });
        var immediate = AccessTools.DeclaredMethod(typeof(Bootstrap), "DestroySceneUi");
        Check(original.Count == rewritten.Count, "Instruction count must not change");
        Check(original.Count(i => i.Calls(deferred)) == 2, "Installed game has two owned UI destruction sites");
        Check(rewritten.Count(i => i.Calls(immediate)) == 2, "Only those two calls change");
        for (int i = 0; i < original.Count; i++)
        {
            Check(original[i].labels.SequenceEqual(rewritten[i].labels), "Preserve branch labels");
            Check(original[i].blocks.SequenceEqual(rewritten[i].blocks), "Preserve exception regions");
            if (!original[i].Calls(deferred))
                Check(original[i].opcode == rewritten[i].opcode && Equals(original[i].operand, rewritten[i].operand), "Preserve other operations");
        }
        foreach (int count in new[] { 0, 1, 3 })
        {
            bool rejected = false;
            try { Bootstrap.Rewrite(Enumerable.Range(0, count).Select(i => new CodeInstruction(OpCodes.Call, deferred))); }
            catch (NotSupportedException) { rejected = true; }
            Check(rejected, "Reject unreviewed destruction layout");
        }
        Console.WriteLine("Flight UI lifetime: " + checks + " IL checks passed; Unity destruction order requires runtime validation.");
    }
}
