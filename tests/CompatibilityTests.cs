using System;
using System.Collections;
using System.IO;
using System.Reflection;
using KerbalOverEngineering.Options;

static class CompatibilityTests
{
    static int checks;
    static void Check(bool condition, string message) { checks++; if (!condition) throw new Exception(message); }
    static int Main(string[] args)
    {
        try
        {
            Check(!CommunityFixes.Present && CommunityFixes.Ready, "No KSPCF: normal initialization");
            Check(CommunityFixes.Reason("Stock.ExhaustSync") == null, "No KSPCF: exhaust available");
            var fixture = Assembly.LoadFile(Path.GetFullPath(args[0]));
            if (args.Length > 1)
            {
                Check(CommunityFixes.Present && !CommunityFixes.Ready, "Unknown API is not treated as absent");
                foreach (var key in new[] { "Stock.ExhaustSync", "Stock.PngPipeline", "Stock.DdsDirectUpload", "Stock.DlcEarlyPreparation", "Stock.PqsNoisePreparation" })
                    Check(CommunityFixes.Reason(key) != null, "Unknown API fails closed for " + key);
                Check(CommunityFixes.Reason("Stock.PortraitFullRate") == null, "Unknown API does not suppress unrelated features");
                Console.WriteLine("UNKNOWN_COMPATIBILITY_API_PASS checks=" + checks); return 0;
            }
            var main = fixture.GetType("KSPCommunityFixes.KSPCommunityFixes");
            Assembly.Load("Assembly-CSharp"); Assembly.Load("0Harmony");
            var setter = fixture.GetType("KSPCommunityFixes.FixtureControl").GetMethod("SetPatch");
            Action<string, bool> setPatch = (name, value) => setter.Invoke(null, new object[] { name, value });
            var registry = (IDictionary)main.GetField("patchInstances").GetValue(null);
            Check(CommunityFixes.Present && !CommunityFixes.Ready, "Installed is not the same as initialized");
            Check(CommunityFixes.Reason("Stock.DlcEarlyPreparation") != null, "Do not build a cache before actual loader ownership is known");
            main.GetProperty("SettingsNode").SetValue(null, new object(), null);
            Check(CommunityFixes.Ready, "Post-load settings available");
            Check(CommunityFixes.Reason("Stock.ExhaustSync") == null, "Installed KSPCF with raycast patch disabled does not suppress KOE");
            var fast = fixture.GetType("KSPCommunityFixes.Performance.KSPCFFastLoader").GetProperty("IsPatchEnabled");
            fast.SetValue(null, true, null);
            Check(CommunityFixes.Reason("Stock.PngPipeline") == null, "A stale enabled flag without an installed patch is insufficient");
            setPatch("FastLoader", true);
            foreach (var key in new[] { "Stock.DdsDirectUpload", "Stock.DdsPrefetch", "Stock.PngPipeline", "Stock.DatabaseChildWaitDrain", "Stock.DatabaseReadyAsyncDrain", "Stock.DatabaseFrameBudget" })
                Check(CommunityFixes.Reason(key) != null, "FastLoader owns " + key);
            Check(CommunityFixes.Reason("EVE.DdsDirectUpload") == null, "EVE loader remains independent");
            Check(CommunityFixes.Reason("Stock.CraftLinkIndex") == null, "Craft linking remains independent");
            fast.SetValue(null, false, null);
            setPatch("FastLoader", false);
            Check(CommunityFixes.Reason("Stock.DdsDirectUpload") == null, "Inactive FastLoader allows stock DDS optimization");
            foreach (var name in new[] { "Performance.OptimizedModuleRaycasts", "Performance.ExpansionBundlePreload", "BugFixes.ScatterDistribution" })
            {
                var type = fixture.GetType("KSPCommunityFixes." + name); registry.Add(type, Activator.CreateInstance(type));
                setPatch(type.Name, true);
            }
            foreach (var key in new[] { "Stock.ExhaustSync", "Stock.ExpansionBufferReuse", "Stock.DlcLz4Cache", "Stock.DlcEarlyPreparation", "Stock.PqsNoisePreparation", "Stock.PqsPatchStateMemo" })
                Check(CommunityFixes.Reason(key) != null, "Only successfully applied patches suppress " + key);
            registry.Clear();
            Check(CommunityFixes.Reason("Stock.ExhaustSync") != null, "Partial failure: actual exhaust patch survives without a success-registry entry");
            Check(CommunityFixes.Reason("Stock.DlcEarlyPreparation") != null, "Actual DLC override survives a missing registry entry");
            foreach (var name in new[] { "OptimizedModuleRaycasts", "ExpansionBundlePreload", "ScatterDistribution" }) setPatch(name, false);
            foreach (var key in new[] { "Stock.ExhaustSync", "Stock.DlcLz4Cache", "Stock.DlcEarlyPreparation", "Stock.PqsNoisePreparation" })
                Check(CommunityFixes.Reason(key) == null, "Disabled/failed KSPCF patch leaves KOE available");
            Check(CommunityFixes.Reason("Stock.PortraitFullRate") == null, "Full-rate portraits remain enabled");
            Check(CommunityFixes.Reason("Stock.PortraitHiddenCull") == null, "Complementary portrait visibility remains enabled");
            var owned = new[] { "Performance.OptimizedModuleRaycasts", "Performance.ExpansionBundlePreload", "BugFixes.ScatterDistribution" };
            for (int mask = 0; mask < 8; mask++) for (int loaderOn = 0; loaderOn < 2; loaderOn++)
            {
                registry.Clear(); fast.SetValue(null, loaderOn != 0, null); setPatch("FastLoader", loaderOn != 0);
                foreach (var name in new[] { "OptimizedModuleRaycasts", "ExpansionBundlePreload", "ScatterDistribution" }) setPatch(name, false);
                for (int i = 0; i < owned.Length; i++) if ((mask & (1 << i)) != 0)
                {
                    var type = fixture.GetType("KSPCommunityFixes." + owned[i]); registry.Add(type, Activator.CreateInstance(type));
                    setPatch(type.Name, true);
                }
                Check((CommunityFixes.Reason("Stock.ExhaustSync") != null) == ((mask & 1) != 0), "Raycast ownership independent of loader and terrain patches");
                Check((CommunityFixes.Reason("Stock.DlcEarlyPreparation") != null) == ((mask & 2) != 0), "Cache preparation follows actual DLC ownership");
                Check((CommunityFixes.Reason("Stock.PqsNoisePreparation") != null) == ((mask & 4) != 0), "PQS ownership independent of loader and DLC patches");
                Check((CommunityFixes.Reason("Stock.PngPipeline") != null) == (loaderOn != 0), "Asset loader ownership independent of other patches");
            }
            Console.WriteLine("COMPATIBILITY_MATRIX_PASS checks=" + checks + "; real Harmony patches on isolated targets; no Unity scene executed"); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
