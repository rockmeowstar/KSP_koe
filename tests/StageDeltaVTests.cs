using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Collections.Generic;
using System.Runtime.Serialization;
using HarmonyLib;
using KoeStageDeltaV;

static class StageDeltaVTests
{
    static string game, repo;
    static int checks;
    static void Check(bool ok, string message)
    {
        checks++;
        if (!ok)
            throw new Exception(message);
    }
    static int Main(string[] args)
    {
        game = args[0];
        repo = args[1];
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            Run();
            Console.WriteLine("Stage delta-V: " + checks + " checks passed");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
    static Assembly Resolve(object s, ResolveEventArgs e)
    {
        string n = new AssemblyName(e.Name).Name + ".dll";
        foreach (string f in new[] { game + "/KSP_x64_Data/Managed", game + "/GameData/000_Harmony", repo + "/dist/GameData/KerbalOverEngineering/Plugins" })
        {
            string p = Path.Combine(f, n);
            if (File.Exists(p))
                return Assembly.Load(File.ReadAllBytes(p));
        }
        return null;
    }
    static void Run()
    {
        var method = AccessTools.DeclaredMethod(typeof(VesselDeltaV), "ResetPartInfo");
        var original = PatchProcessor.GetOriginalInstructions(method).ToList();
        var rewritten = Bootstrap.Rewrite(original).ToList();
        Check(rewritten.Count == original.Count - 24, "Only search blocks replaced");
        foreach (string name in new[] { "CalculateMassValues", "CalculateStagingValues" })
        {
            var m = AccessTools.Method(typeof(DeltaVPartInfo), name);
            Check(rewritten.Count(x => x.Calls(m)) == original.Count(x => x.Calls(m)), name + " retained");
        }
        var labels = new HashSet<Label>(rewritten.SelectMany(x => x.labels));
        foreach (var x in rewritten)
        {
            if (x.operand is Label)
                Check(labels.Contains((Label)x.operand), "Branch target retained");
            var a = x.operand as Label[];
            if (a != null)
            foreach (var l in a)
                Check(labels.Contains(l), "Switch target retained");
        }
        var altered = original.Select(x => new CodeInstruction(x)).ToList();
        altered[83].operand = null;
        bool rejected = false;
        try
        {
            Bootstrap.Rewrite(altered).ToList();
        }
        catch (NotSupportedException) { rejected = true; }
        Check(rejected, "Different reset implementation rejected");
        // AI-generated comment: This Unity build compares nonnull wrappers by reference; cached pointers mark synthetic liveness only.
        var aPart = PartWithId(41);
        var bPart = PartWithId(42);
        var alias = PartWithId(41);
        var aInfo = (DeltaVPartInfo)FormatterServices.GetUninitializedObject(typeof(DeltaVPartInfo));
        aInfo.part = aPart;
        var bInfo = (DeltaVPartInfo)FormatterServices.GetUninitializedObject(typeof(DeltaVPartInfo));
        bInfo.part = bPart;
        var duplicate = (DeltaVPartInfo)FormatterServices.GetUninitializedObject(typeof(DeltaVPartInfo));
        duplicate.part = aPart;
        var list = new List<DeltaVPartInfo> { aInfo, bInfo, duplicate };
        object[] call = { new List<Part> { aPart, bPart }, null };
        AccessTools.Method(typeof(Bootstrap), "Enter").Invoke(null, call);
        try
        {
            Check(Bootstrap.Find(list, aPart) == 0, "First duplicate reference wins");
            Check(Bootstrap.Find(list, alias) == -1, "Different wrapper remains distinct in shipped Unity");
            var custom = (EqualPart)FormatterServices.GetUninitializedObject(typeof(EqualPart));
            AccessTools.Field(typeof(UnityEngine.Object), "m_CachedPtr").SetValue(custom, new IntPtr(44));
            var customInfo = (DeltaVPartInfo)FormatterServices.GetUninitializedObject(typeof(DeltaVPartInfo));
            customInfo.part = custom;
            list.Insert(0, customInfo);
            Check(Bootstrap.Find(list, aPart) == 1, "Overridden object equality does not replace Unity operator semantics");
            list.RemoveAt(0);
            Check(Bootstrap.Find(list, bPart) == 1, "Other part found");
            Check(Bootstrap.Find(list, PartWithId(43)) == -1, "Missing part");
            list.RemoveAt(0);
            Check(Bootstrap.Find(list, aPart) == 1, "Removal invalidates index");
            list.Insert(0, aInfo);
            Check(Bootstrap.Find(list, aPart) == 0, "Insertion invalidates index");
            list.Reverse();
            Check(Bootstrap.Find(list, aPart) == 0, "Reordering retains first match");
            Check(Bootstrap.Contains((List<Part>)call[0], aPart), "Membership uses shipped Unity equality");
            Check(!Bootstrap.Contains((List<Part>)call[0], alias), "Distinct wrapper is not a member");
            Check(!Bootstrap.Contains((List<Part>)call[0], PartWithId(43)), "Missing membership");
        }
        finally { AccessTools.Method(typeof(Bootstrap), "Leave").Invoke(null, new[] { call[1], null }); }
    }
    static Part PartWithId(int id)
    {
        var p = (Part)FormatterServices.GetUninitializedObject(typeof(Part));
        AccessTools.Field(typeof(UnityEngine.Object), "m_CachedPtr").SetValue(p, new IntPtr(id));
        return p;
    }
    sealed class EqualPart : Part
    {
        public override bool Equals(object other) { return true; }
        public override int GetHashCode() { return 0; }
    }
}
