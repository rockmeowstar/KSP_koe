using System;
using System.IO;
using System.Linq;
using KerbalOverEngineering.Options;

static class OptionStoreTests
{
    static void Check(bool condition, string message)
    {
        if (!condition)
            throw new Exception(message);
    }

    static void Main()
    {
        string root = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "KOE-options-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        string path = System.IO.Path.Combine(root, "Options.cfg");
        try
        {
            Check(
                Catalog.Features.Length == 75
                && Catalog.Features.Select(f => f.Key).Distinct().Count() == 75,
                "The option catalog must contain 75 distinct feature keys."
            );
            var first = new OptionStore(path, f => f.Group == "EVE" ? "EVE missing" : null);
            Check(first.Enabled("Rendering.ShaderConstantUpdates"), "Supported shader constants enabled on first config creation");
            Check(first.Enabled("Stock.FlightUiLifecycle"), "UI lifecycle repair defaults on");
            Check(first.Enabled("Stock.PlanetLaunchReadiness"), "Launch readiness defaults on");
            Check(first.Enabled("Rendering.BlendFactorNormalization"), "Blend-factor normalization defaults on with its point-light dependency");
            Check(first.Enabled("Rendering.ShadowDrawBatching"), "Shadow draw batching defaults on");
            Check(first.Enabled("Rendering.RustShaderSubmission"), "Accepted Rust submission defaults on when supported");
            Check(Catalog.Features.First(f => f.Key == "EVE.CloudShaderPacking").DefaultEnabled && !first.Enabled("EVE.CloudShaderPacking"),
                "Accepted cloud shader packing defaults on but still requires EVE");
            Check(!first.Enabled("EVE.CloudMapPacking") && !Catalog.Features.First(f => f.Key == "EVE.CloudMapPacking").DefaultEnabled,
                "Cloud map packing remains opt-in");
            Check(first.Enabled("Stock.DeltaVResetIndex"), "Accepted delta-V reset index defaults on when supported");
            Check(first.Enabled("Stock.RealtimePhysicsCatchup"), "Accepted real-time physics catch-up defaults on");
            Check(first.Enabled("Stock.PhysicsWarpPrecision"), "Physical warp precision defaults on when catch-up is supported");
            Check(!Catalog.Features.Any(f => f.Key == "Stock.AxisGroupParallel" || f.Key == "KSPCommunityFixes.RendererTreeBatch"), "Unaccepted parallel experiments are absent from the release catalog");
            Check(first.Enabled("FAR.ExposurePoolReset") && first.Enabled("Stock.OverlayCameraCleanup") && first.Enabled("Deferred.SsrResourceCleanup"), "Supported lifetime corrections default on");
            Check(first.Enabled("CameraTools.ToolbarLifetime"), "Supported toolbar ownership correction defaults on");
            Check(first.Enabled("Scatterer.ShadowCopyLifetime"), "Conditional shadow resource cleanup defaults on after user acceptance");
            Check(first.Enabled("Scatterer.ScreenCopyReuse"), "Screen-copy reuse defaults on after explicit user acceptance");
            Check(first.Enabled("FAR.RustRendererFilter"), "Accepted native FAR filter defaults on when supported");
            Check(first.Enabled("FAR.RustRendererSetup"), "Accepted native FAR property preparation defaults on when supported");
            Check(first.Created && first.Enabled("Stock.ExhaustSync") && !first.Enabled("EVE.CurveBakeHoist"), "Detect and generate");
            string manual = File.ReadAllText(path).Replace("ExhaustSync = true", "ExhaustSync = false").Replace("RustRendererFilter = true", "RustRendererFilter = false") + "\n// user comment\n";
            File.WriteAllText(path, manual);
            byte[] before = File.ReadAllBytes(path);
            var second = new OptionStore(path, f => null);
            Check(
                !second.Created
                && !second.Enabled("Stock.ExhaustSync")
                && !second.Enabled("EVE.CurveBakeHoist"),
                "Preserve choices after dependency change"
            );
            Check(before.SequenceEqual(File.ReadAllBytes(path)), "Existing bytes preserved");
            Check(!second.Enabled("FAR.RustRendererFilter") && second.Enabled("FAR.RustRendererSetup"), "FAR options preserve independent manual choices");
            File.Delete(path);
            var regenerated = new OptionStore(path, f => null);
            Check(!regenerated.Enabled("EVE.CloudMapPacking"), "Fresh compatible installations leave cloud map packing disabled");
            Check(Catalog.Features.First(f => f.Key == "EVE.CloudPassFusion").DefaultEnabled && regenerated.Enabled("EVE.CloudPassFusion"),
                "Accepted pass fusion defaults on with its supported shader-packing parent");
            Check(
                regenerated.Created
                && regenerated.Enabled("Stock.ExhaustSync")
                && regenerated.Enabled("EVE.CurveBakeHoist"),
                "Delete causes fresh detection"
            );
            Check(
                !File.ReadAllText(path).Contains("SimulatedTransferKernel")
                && !File.ReadAllText(path).Contains("ViewCoverageCull")
                && !regenerated.Enabled("Rendering.FrameGeneration")
                && !regenerated.Enabled("Rendering.AdaptiveFrameCadence"),
                "Frame generation defaults off and also disables its dependent cadence option"
            );
            var lostDependency = new OptionStore(path, f => f.Group == "EVE" ? "EVE removed" : null);
            Check(!lostDependency.Enabled("EVE.CurveBakeHoist"), "Runtime dependency guard beats true");
            var unsupportedNative = new OptionStore(path, f => f.Requirement == "FarNative" ? "Unsupported native binding" : null);
            Check(!unsupportedNative.Enabled("FAR.RustRendererFilter") && !unsupportedNative.Enabled("FAR.RustRendererSetup"), "Unsupported FAR bindings retain their original path");
            string original = File.ReadAllText(path);
            File.WriteAllText(path, original.Replace("RealtimePhysicsCatchup = true", "RealtimePhysicsCatchup = false"));
            Check(!new OptionStore(path, f => null).Enabled("Stock.RealtimePhysicsCatchup"), "Preserve a manually disabled catch-up option");
            Check(!new OptionStore(path, f => null).Enabled("Stock.PhysicsWarpPrecision"), "Warp precision requires its catch-up parent");
            File.WriteAllText(path, original.Replace("RealtimePhysicsCatchup = true", ""));
            Check(!new OptionStore(path, f => null).Enabled("Stock.RealtimePhysicsCatchup"), "Existing configurations require an explicit catch-up flag");
            File.WriteAllText(path, original);
            string legacy = original.Replace("CloudMapPacking = false", "");
            File.WriteAllText(path, legacy);
            Check(!new OptionStore(path, f => null).Enabled("EVE.CloudMapPacking"), "An existing config needs an explicit new map-packing flag");
            Check(File.ReadAllText(path) == legacy, "Adding a feature never rewrites an existing config");
            File.WriteAllText(path, original);
            foreach (var pair in new[]
            {
                new[] { "CloudShaderPacking", "CloudPassFusion", "EVE" },
                new[] { "InstancedPointLights", "BlendFactorNormalization", "Rendering" },
                new[]
                {
                    "FrameGeneration",
                    "AdaptiveFrameCadence",
                    "Rendering"
                },
                new[]
                {
                    "PropellantMessageCache",
                    "PropellantArgumentReuse",
                    "Stock"
                }
            }

            )
            {
                string enabled = original.Replace(pair[0] + " = false", pair[0] + " = true").Replace(pair[1] + " = false", pair[1] + " = true");
                File.WriteAllText(path, enabled);
                Check(new OptionStore(path, f => null).Enabled(pair[2] + "." + pair[1]), "Independent child enabled: " + pair[1]);
                File.WriteAllText(path, enabled.Replace(pair[0] + " = true", pair[0] + " = false"));
                Check(!new OptionStore(path, f => null).Enabled(pair[2] + "." + pair[1]), "Parent disables child: " + pair[1]);
            }

            File.WriteAllText(path, original.Replace("DlcLz4Cache = true", "DlcLz4Cache = false"));
            Check(!new OptionStore(path, f => null).Enabled("Stock.DlcEarlyPreparation"), "Parent disabled");
            File.Delete(path);
            var upstream = new OptionStore(path, f => null, f => f.Key == "Stock.ExhaustSync" ? "replacement in installed upstream version" : null);
            Check(!upstream.Enabled("Stock.ExhaustSync")
                && upstream.Enabled("Stock.PortraitFullRate"), "Only implemented upstream feature defaults off");
            Check(File.ReadAllText(path).Contains("// Default off: replacement in installed upstream version"), "Explain upstream default");
            File.WriteAllText(path, File.ReadAllText(path).Replace("ExhaustSync = false", "ExhaustSync = true"));
            before = File.ReadAllBytes(path);
            Check(new OptionStore(path, f => null, f => "new upstream replacement").Enabled("Stock.ExhaustSync"), "Existing manual opt-in is preserved");
            Check(before.SequenceEqual(File.ReadAllBytes(path)), "Default detection never rewrites manual settings");
            File.WriteAllText(path, original + "\n[Stock]\nExhaustSync = true\n");
            Check(!new OptionStore(path, f => null).Enabled("Stock.ExhaustSync"), "Ambiguous duplicate fails closed");
            File.WriteAllText(path, original.Replace("schemaVersion = 1", "schemaVersion = 2"));
            Check(Catalog.Features.All(f => !new OptionStore(path, x => null).Enabled(f.Key)), "Unsupported schema fails closed");
            Check(File.ReadAllText(path).Contains("schemaVersion = 2"), "Never repair by overwriting");
            File.Delete(path);
            int creators = 0;
            System.Threading.Tasks.Parallel.For(0, 8, i =>
            {
                var competing = new OptionStore(path, f => null);
                if (competing.Created)
                    System.Threading.Interlocked.Increment(ref creators);
                Check(Catalog.Features.All(f => competing.Enabled(f.Key) == (f.DefaultEnabled && (f.Parent==null || competing.Enabled(f.Parent)))), "Atomic readers see complete defaults including disabled parents");
            });
            Check(creators == 1, "Exactly one creator wins");
            File.WriteAllBytes(path, new byte[0]);
            var empty = new OptionStore(path, f => null);
            Check(
                !empty.Created
                && Catalog.Features.All(f => !empty.Enabled(f.Key))
                && new FileInfo(path).Length == 0,
                "Empty manual file preserved and disabled"
            );
            string blocked = System.IO.Path.Combine(root, "blocked.cfg");
            Directory.CreateDirectory(blocked);
            bool failed = false;
            try
            {
                new OptionStore(blocked, f => null);
            }
            catch (IOException)
            {
                failed = true;
            }

            Check(failed && Directory.Exists(blocked), "Filesystem failure surfaces without replacing user directory");
            Check(Directory.GetFiles(root, "*.tmp").Length == 0, "Failed config publication cleans temporary file");
            Console.WriteLine("Options: " + Catalog.Features.Length + " feature keys checked; config creation, edits, dependencies and recovery passed.");
        }
        finally
        {
            Directory.Delete(root, true);
        }
    }
}
