using System;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;

static class SettingsCompatibilityTests
{
    static string managed;
    static Type support, feature;

    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string path = Path.Combine(managed, new AssemblyName(args.Name).Name + ".dll");
        return File.Exists(path) ? Assembly.LoadFrom(path) : null;
    }

    static void Check(string requirement, string expected)
    {
        object probe = Activator.CreateInstance(support, true);
        object option = Activator.CreateInstance(feature, new object[] { "Test", "Compatibility", requirement, null, true });
        string actual = (string)support.GetMethod("Check").Invoke(probe, new[] { option });
        if (actual != expected)
            throw new Exception(requirement + ": expected " + (expected ?? "supported") + ", got " + (actual ?? "supported"));
    }

    static Assembly FakeAssembly(string name)
    {
        var assembly = AppDomain.CurrentDomain.DefineDynamicAssembly(new AssemblyName(name), AssemblyBuilderAccess.Run);
        assembly.DefineDynamicModule(name);
        return assembly;
    }

    static void Main(string[] args)
    {
        managed = args[1];
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        Assembly options = Assembly.LoadFrom(Path.GetFullPath(args[0]));
        support = options.GetType("KerbalOverEngineering.Options.Settings", true).GetNestedType("Support", BindingFlags.NonPublic);
        feature = options.GetType("KerbalOverEngineering.Options.Feature", true);
        Check("Harmony", "Assembly-CSharp is missing");
        Assembly game = FakeAssembly("Assembly-CSharp");
        if (game.ManifestModule.ModuleVersionId == new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
            throw new Exception("Test assembly must have a different MVID");
        Check("Harmony", "0Harmony is missing");
        FakeAssembly("0Harmony");
        Check("Harmony", null);
        Check("Deferred", "Deferred is missing");
        Console.WriteLine("Settings compatibility: different game MVID accepted; missing game, Harmony and Deferred still rejected.");
    }
}
