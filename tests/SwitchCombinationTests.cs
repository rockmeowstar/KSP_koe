using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using HarmonyLib;
using KerbalOverEngineering.Options;

static class SwitchCombinationTests
{
    static string game;
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (string folder in new[]
        {
            "KSP_x64_Data/Managed",
            "GameData/000_Harmony",
            "GameData/EnvironmentalVisualEnhancements/Plugins"
        }

        )
        {
            string path = Path.Combine(game, folder, name);
            if (File.Exists(path))
                return Assembly.Load(File.ReadAllBytes(path));
        }

        return null;
    }

    static void Check(bool value, string message)
    {
        if (!value)
            throw new Exception(message);
    }

    static int Main(string[] args)
    {
        game = args[0];
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            Run();
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        var stock = AccessTools.DeclaredMethod(typeof(DatabaseLoaderTexture_DDS), "Read");
        var utils = Assembly.Load(File.ReadAllBytes(Path.Combine(game, "GameData/EnvironmentalVisualEnhancements/Plugins/Utils.dll")));
        var eve = AccessTools.DeclaredMethod(utils.GetType("Utils.TextureConverter", true), "DDSToTexture");
        for (int direct = 0; direct < 2; direct++)
            for (int prefetch = 0; prefetch < 2; prefetch++)
            {
                Settings.Disabled.Clear();
                foreach (string prefix in new[]
                {
                    "Stock",
                    "EVE"
                }

                )
                {
                    if (direct == 0)
                        Settings.Disabled.Add(prefix + ".DdsDirectUpload");
                    if (prefetch == 0)
                        Settings.Disabled.Add(prefix + (prefix == "Stock" ? ".DdsPrefetch" : ".TexturePrefetch"));
                }

                var stockOriginal = PatchProcessor.GetOriginalInstructions(stock).ToList();
                var stockCode = DdsUploadFix.Patch.Transpile(stockOriginal).ToList();
                Check(stockCode.Count(i => i.Calls(AccessTools.Method(typeof(DdsUploadFix.Patch), "Upload"))) == direct * 2, "Stock copy switch");
                Check(stockCode.Count(i => i.Calls(AccessTools.Method(typeof(DdsUploadFix.PrefetchPhase), "Read"))) == prefetch, "Stock prefetch switch");
                var eveCode = CloudLoopFix.EveTexturePatch.Transpile(PatchProcessor.GetOriginalInstructions(eve)).ToList();
                Check(eveCode.Count(i => i.Calls(AccessTools.Method(typeof(CloudLoopFix.EveTexturePatch), "Upload"))) == direct * 14, "EVE copy switch");
                Check(eveCode.Count(i => i.Calls(AccessTools.Method(typeof(CloudLoopFix.EveTexturePatch), "Read"))) == prefetch, "EVE prefetch switch");
                if (direct == 0 && prefetch == 0)
                {
                    var original = stockOriginal;
                    Check(
                        stockCode.Count == original.Count
                        && stockCode.Zip(original, (a, b) => a.opcode == b.opcode
                        && Equals(a.operand, b.operand)).All(x => x),
                        "Both disabled retains original stock IL"
                    );
                }
            }

        Console.WriteLine("DDS options: all four copy/prefetch combinations passed.");
    }
}
