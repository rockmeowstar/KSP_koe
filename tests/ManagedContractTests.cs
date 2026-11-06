using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using KerbalOverEngineering.Options;

static class ManagedContractTests
{
    static int checks;
    static void Check(bool ok, string message)
    {
        checks++;
        if (!ok) throw new Exception(message);
    }

    static MethodInfo Fixture(bool extraMetadata, int value, bool otherField = false)
    {
        var assembly = AppDomain.CurrentDomain.DefineDynamicAssembly(new AssemblyName("ModFixture"), AssemblyBuilderAccess.Run);
        var module = assembly.DefineDynamicModule("Fixture");
        var type = module.DefineType("Mod.Algorithm", TypeAttributes.Public);
        if (extraMetadata) type.DefineField("Unrelated", typeof(double), FieldAttributes.Public);
        var field = type.DefineField(otherField ? "Different" : "Input", typeof(int), FieldAttributes.Public | FieldAttributes.Static);
        var method = type.DefineMethod("Compute", MethodAttributes.Public | MethodAttributes.Static, typeof(int), Type.EmptyTypes);
        var il = method.GetILGenerator();
        il.Emit(OpCodes.Ldsfld, field);
        il.Emit(OpCodes.Ldc_I4, value);
        il.Emit(OpCodes.Add);
        il.Emit(OpCodes.Ret);
        return type.CreateType().GetMethod("Compute");
    }

    static Assembly Resolve(string game, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (var folder in new[] { "KSP_x64_Data/Managed", "GameData/EnvironmentalVisualEnhancements/Plugins", "GameData/000_Harmony", "GameData/ParallaxContinued/Plugins", "GameData/KSPTextureLoader/Plugins", "GameData/Kopernicus/Plugins" })
        {
            string path = Path.Combine(game, folder, name);
            if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
        }
        return null;
    }

    static void ChangedModMvid(string path, Assembly original)
    {
        byte[] bytes = File.ReadAllBytes(path), old = original.ManifestModule.ModuleVersionId.ToByteArray();
        int offset = -1;
        for (int at = 0; at <= bytes.Length - old.Length; at++)
        {
            bool match = true;
            for (int i = 0; i < old.Length; i++) if (bytes[at + i] != old[i]) { match = false; break; }
            if (!match) continue;
            Check(offset == -1, "Fixture MVID must occur once"); offset = at;
        }
        Check(offset >= 0, "Find MVID in a private byte copy");
        Array.Copy(Guid.NewGuid().ToByteArray(), 0, bytes, offset, old.Length);
        var rebuilt = Assembly.Load(bytes);
        Check(rebuilt.ManifestModule.ModuleVersionId != original.ManifestModule.ModuleVersionId, "Real mod MVID must differ");
        string type = "Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer";
        var flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
        foreach (var method in new[] { "ResolveLayerOverlapIntervals", "HandleRenderingCommands" })
            Check(ManagedContract.Fingerprint(original.GetType(type).GetMethod(method, flags))
                == ManagedContract.Fingerprint(rebuilt.GetType(type).GetMethod(method, flags)), "Real EVE algorithm must accept another MVID");
    }

    static int Main(string[] args)
    {
        try
        {
            var a = Fixture(false, 7);
            var b = Fixture(true, 7);
            Check(a.Module.ModuleVersionId != b.Module.ModuleVersionId, "Fixtures must have different MVIDs");
            Check(!a.GetMethodBody().GetILAsByteArray().SequenceEqual(b.GetMethodBody().GetILAsByteArray()), "Fixture tokens must differ");
            Check(ManagedContract.Fingerprint(a) == ManagedContract.Fingerprint(b), "Equivalent rebuilt method must pass");
            Check(ManagedContract.Fingerprint(a) != ManagedContract.Fingerprint(Fixture(false, 8)), "Changed constant must fail");
            Check(ManagedContract.Fingerprint(a) != ManagedContract.Fingerprint(Fixture(false, 7, true)), "Changed field must fail");
            bool rejected = false;
            try { ManagedContract.Require(Fixture(false, 8), ManagedContract.Fingerprint(a)); }
            catch (NotSupportedException) { rejected = true; }
            Check(rejected, "Changed algorithm must fail closed");
            Console.WriteLine("Managed contracts: " + checks + " checks passed");
            if (args.Length == 0) return 0;
            string game = args[0];
            AppDomain.CurrentDomain.AssemblyResolve += (sender, e) => Resolve(game, e);
            var kopernicus = Resolve(game, new ResolveEventArgs("Kopernicus"));
            Check(ManagedContract.IndexedBiomeProblem(kopernicus.GetType("Kopernicus.Components.KopernicusCBAttributeMapSO", true)) != null,
                "Current Kopernicus retains its own texture compiler");
            foreach (string name in new[] { "Atmosphere", "ParallaxContinued", "Utils" })
            {
                var assembly = Resolve(game, new ResolveEventArgs(name));
                if (name == "Utils")
                {
                    var atmosphere = Resolve(game, new ResolveEventArgs("Atmosphere"));
                    string signatures = string.Join("; ", assembly.GetType("Utils.TextureConverter").GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                        .Where(m => m.Name == "DDSToTexture").Select(m => m.ReturnType.FullName + "(" + string.Join(",", m.GetParameters().Select(p => p.ParameterType.FullName).ToArray()) + ")").ToArray());
                    Check(ManagedContract.EveDdsLoader(assembly) != null, "Installed EVE DDS signature: " + signatures);
                    foreach (string feature in new[] { "TexturePrefetch", "DdsDirectUpload" })
                        Check(ManagedContract.EveProblem(atmosphere, assembly, feature) == null, "Installed EVE must retain " + feature);
                }
                if (name == "Atmosphere") ChangedModMvid(Path.Combine(game, "GameData/EnvironmentalVisualEnhancements/Plugins/Atmosphere.dll"), assembly);
                if (name == "ParallaxContinued")
                    foreach (var feature in new[] { "GpuSkyboxCopy", "LaunchTextureRetention", "BiomeRgbPreparation", "DdsDirectUpload" })
                        Check(ManagedContract.ParallaxProblem(assembly, feature) != null, "Upstream Parallax pipeline must suppress redundant " + feature);
                if (args.Length < 2) continue;
                Console.WriteLine(name + " version=" + assembly.GetName().Version + " references=" + string.Join(",", assembly.GetReferencedAssemblies().Select(r => r.Name).ToArray()));
                foreach (var type in new[] { "Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer", "Atmosphere.CelestialBodyCloudsHandler", "Atmosphere.CloudsRaymarchedVolume+<>c__DisplayClass154_0", "Utils.TextureConverter" }.Select(n => assembly.GetType(n, false)).Where(t => t != null))
                    foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly)
                        .Where(m => new[] { "ResolveLayerOverlapIntervals", "LoadBody", "Update", "ApplyConfigNode", "DDSToTexture", "<BakeCurvesTexture>b__0" }.Contains(m.Name)))
                        Console.WriteLine(type.FullName + "." + method.Name + "=" + ManagedContract.Fingerprint(method));
            }
            Console.WriteLine("Managed contracts and installed mod APIs: " + checks + " checks passed");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
