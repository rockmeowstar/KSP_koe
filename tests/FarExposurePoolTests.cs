using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.Serialization;
using HarmonyLib;
using KoeFarEventPath;
using UnityEngine;

static class FarExposurePoolTests
{
    static string game, repo;
    static int checks;
    static void Check(bool value, string message) { checks++; if (!value) throw new Exception(message); }
    static int Main(string[] args)
    {
        game = args[0]; repo = args[1]; AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try { Run(); Console.WriteLine("FAR exposure pool: " + checks + " checks passed against installed FAR"); return 0; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        var loaded = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(x => x.GetName().Name == name);
        if (loaded != null) return loaded;
        foreach (string folder in new[] { game + "/KSP_x64_Data/Managed", game + "/GameData/000_Harmony", game + "/GameData/FerramAerospaceResearch/Plugins", game + "/GameData/000_KSPBurst/Plugins", game + "/GameData/KerbalOverEngineering/Plugins" })
        { string path = Path.Combine(folder, name + ".dll"); if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path)); }
        return null;
    }
    static object Owner(Type type)
    {
        object owner = FormatterServices.GetUninitializedObject(type); GC.SuppressFinalize(owner);
        var objects = AccessTools.Field(type, "objects"); objects.SetValue(owner, Activator.CreateInstance(objects.FieldType));
        var slots = AccessTools.Field(type, "renderers"); slots.SetValue(owner, Activator.CreateInstance(slots.FieldType));
        return owner;
    }
    static void Populate(Type type, object owner, int count, object pool)
    {
        var objects = (IDictionary)AccessTools.Field(type, "objects").GetValue(owner);
        var slots = (IList)AccessTools.Field(type, "renderers").GetValue(owner);
        var acquire = AccessTools.Method(pool.GetType(), "Acquire");
        for (int i = 0; i < count; i++)
        {
            var part = (Part)FormatterServices.GetUninitializedObject(typeof(Part));
            objects.Add(part, i); slots.Add(acquire.Invoke(pool, null));
        }
    }
    static void Run()
    {
        var assembly = Assembly.Load(File.ReadAllBytes(game + "/GameData/FerramAerospaceResearch/Plugins/FerramAerospaceResearch.Base.dll"));
        var type = assembly.GetType("FerramAerospaceResearch.Geometry.Exposure.Renderer`1").MakeGenericType(typeof(Part));
        var reset = AccessTools.Method(type, "Reset");
        var pool = AccessTools.Field(type, "renderersPool").GetValue(null);
        AccessTools.Method(pool.GetType(), "Clear").Invoke(pool, null);
        var broken = Owner(type); Populate(type, broken, 3, pool); reset.Invoke(broken, null);
        Check(((IList)AccessTools.Field(type, "renderers").GetValue(broken)).Count == 3, "Installed upstream reset leaves released slots behind");
        Check(((IDictionary)AccessTools.Field(type, "objects").GetValue(broken)).Count == 0, "Upstream object registry is empty while slots remain");
        AccessTools.Method(pool.GetType(), "Clear").Invoke(pool, null);
        ExposurePool.Prepare(type);
        var original = PatchProcessor.GetOriginalInstructions(reset).ToList();
        Check(ExposurePool.NeedsFix(original), "Missing slot clear recognized without an MVID pin");
        var fixedShape = original.Concat(new[] { new CodeInstruction(OpCodes.Callvirt, AccessTools.Method(typeof(List<HashSet<Renderer>>), "Clear")) });
        Check(!ExposurePool.NeedsFix(fixedShape), "Upstream slot clear disables duplicate fix");
        var owner = Owner(type);
        for (int cycle = 0; cycle < 80; cycle++)
        {
            Populate(type, owner, 1 + cycle % 17, pool);
            ExposurePool.Reset(owner);
            Check(((IList)AccessTools.Field(type, "renderers").GetValue(owner)).Count == 0, "Released slots are no longer owned by renderer");
            Check(((IDictionary)AccessTools.Field(type, "objects").GetValue(owner)).Count == 0, "Object registry remains empty after reset");
        }
        var owners = new[] { Owner(type), Owner(type), Owner(type) };
        for (int cycle = 0; cycle < 90; cycle++)
        {
            int current = cycle % owners.Length;
            ExposurePool.Reset(owners[current]);
            Populate(type, owners[current], 1 + cycle % 13, pool);
            var activeSets = new HashSet<HashSet<Renderer>>();
            foreach (var vessel in owners)
                foreach (HashSet<Renderer> set in (IList)AccessTools.Field(type, "renderers").GetValue(vessel))
                    Check(activeSets.Add(set), "Concurrent vessel owners must not share a pooled renderer set");
        }
        foreach (var vessel in owners) ExposurePool.Reset(vessel);
        long before = ExposurePool.LegacyFallbacks;
        var legacy = Owner(type); Populate(type, legacy, 2, pool); reset.Invoke(legacy, null); Populate(type, legacy, 1, pool);
        ExposurePool.Reset(legacy);
        Check(ExposurePool.LegacyFallbacks == before + 1, "Already-corrupted owner retains original reset path");
        Check(((IList)AccessTools.Field(type, "renderers").GetValue(legacy)).Count == 3, "Legacy fallback does not silently change existing ownership");
        var far = Assembly.Load(File.ReadAllBytes(game + "/GameData/FerramAerospaceResearch/Plugins/FerramAerospaceResearch.dll"));
        var caller = AccessTools.Method(far.GetType("FerramAerospaceResearch.FARAeroComponents.VehicleExposure"), "ResetParts").MakeGenericMethod(typeof(List<Part>));
        var code = PatchProcessor.GetOriginalInstructions(caller).ToList();
        var rewritten = ExposurePool.Rewrite(code).ToList();
        Check(code.Count == rewritten.Count, "Only the reset callsite changes");
        Check(rewritten.Count(x => x.Calls(AccessTools.Method(typeof(ExposurePool), "Reset"))) == 1, "One fixed reset call composes with the existing renderer-init transpiler");
    }
}
