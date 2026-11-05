using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using HarmonyLib;
using UnityEngine;

static class EventPathTests
{
    static string game, root;
    static int checks;
    static void Check(bool value, string message)
    {
        checks++;
        if (!value)
            throw new Exception(message);
    }

    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string file = new AssemblyName(args.Name).Name + ".dll";
        foreach (string folder in new[]
        {
            "KSP_x64_Data/Managed",
            "GameData/000_Harmony",
            "GameData/FerramAerospaceResearch/Plugins",
            "GameData/FreeIva/Plugins",
            "GameData/Waterfall/Plugins",
            "GameData/KSPCommunityFixes/Plugins",
            "GameData/000_KSPBurst/Plugins"
        }

        )
        {
            string path = Path.Combine(game, folder, file);
            if (File.Exists(path))
                return Assembly.Load(File.ReadAllBytes(path));
        }

        if (file == "KerbalOverEngineering.Options.dll")
            return Assembly.Load(File.ReadAllBytes(Path.Combine(root, "dist/GameData/KerbalOverEngineering/Plugins", file)));
        string[] paths = Directory.GetFiles(Path.Combine(game, "GameData"), "*" + file, SearchOption.AllDirectories);
        if (paths.Length == 1)
            return Assembly.Load(File.ReadAllBytes(paths[0]));
        return null;
    }

    static int Main(string[] args)
    {
        try
        {
            game = args[0];
            root = args.Length > 1 ? args[1] : Environment.GetEnvironmentVariable("KOE_EVENT_TEST_ROOT");
            AppDomain.CurrentDomain.AssemblyResolve += Resolve;
            Run();
            Console.WriteLine("Event paths: " + checks + " static checks passed.");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    static MethodInfo Method(Type type, string name)
    {
        return AccessTools.DeclaredMethod(type, name);
    }

    static void Negative(Action action, string name)
    {
        bool rejected = false;
        try
        {
            action();
        }
        catch (NotSupportedException)
        {
            rejected = true;
        }

        Check(rejected, name);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        var request = Method(typeof(ModuleEngines), "RequestPropellant");
        var old = PatchProcessor.GetOriginalInstructions(request);
        var changed = PropellantRequestFix.Patch.TranspileOne(old).ToList();
        Check(changed.Count == old.Count - 5, "Exactly five array-construction instructions removed");
        Check(changed.Count(c => c.opcode == OpCodes.Newarr
            && Equals(c.operand, typeof(string))) == 0, "No deprivation argument array remains");
        Check(changed.Any(c => c.Calls(Method(typeof(PropellantRequestFix.Patch), "FormatMessageOne"))), "Single-value cache call bound");
        Negative(() => PropellantRequestFix.Patch.TranspileOne(new CodeInstruction[0]).ToList(), "Unknown fuel IL rejected");
        Check(PropellantRequestFix.Patch.TranspileLegacy(old).Count() == old.Count, "Disabled argument optimization retains baseline IL shape");
        var stage = Method(typeof(KSP.UI.Screens.StageManager), "ActivateStage");
        var stageOld = PatchProcessor.GetOriginalInstructions(stage);
        var stageNew = KoeEventPathFix.Bootstrap.StageLists(stageOld).ToList();
        Check(stageOld.Count == stageNew.Count, "Staging control-flow length preserved");
        Check(stageNew.Count(c => c.Calls(Method(typeof(KoeEventPathFix.Bootstrap), "RentStage"))) == 1, "Single stage allocation replaced");
        Negative(() => KoeEventPathFix.Bootstrap.StageLists(new CodeInstruction[0]).ToList(), "Unknown stage IL rejected");
        var dm = new DynamicMethod("GaugeValidation", typeof(void), Type.EmptyTypes);
        var gauge = PatchProcessor.GetOriginalInstructions(Method(typeof(ModuleEngines), "UpdatePropellantGauge"), dm.GetILGenerator());
        var gaugeNew = KoeEventPathFix.Bootstrap.GaugeLookup(gauge, dm.GetILGenerator()).ToList();
        Check(gaugeNew.Any(c => c.operand is MethodInfo && ((MethodInfo)c.operand).Name == "TryGetValue"), "Gauge lookup changed");
        Negative(() => KoeEventPathFix.Bootstrap.GaugeLookup(new CodeInstruction[0], dm.GetILGenerator())
            .ToList(), "Unknown gauge IL rejected");
        foreach (var item in new[]
        {
            new[]
            {
                "KSPCommunityFixes",
                "KSPCommunityFixes.Performance.CollisionManagerFastUpdate",
                "UpdatePartCollisionIgnoresFast"
            },
            new[]
            {
                "FreeIva",
                "FreeIva.FreeIvaHatch",
                "RefreshConnection"
            },
            new[]
            {
                "Waterfall",
                "Waterfall.ModuleWaterfallFX",
                "GatherRenderers"
            }
        }

        )
        {
            var assembly = Resolve(null, new ResolveEventArgs(item[0]));
            var type = assembly.GetType(item[1], true);
            var instructions = PatchProcessor.GetOriginalInstructions(Method(type, item[2]));
            var result = item[0] == "KSPCommunityFixes" ? KoeCollisionStateFix.Bootstrap.Transpile(instructions) : item[0] == "FreeIva" ? KoeFreeIvaRefresh.Bootstrap.Transpile(instructions) : KoeWaterfallRendererIndex.Bootstrap.Transpile(instructions);
            Check(result.Count() == instructions.Count, "One signature-preserving call substitution: " + item[0]);
        }

        Negative(() => KoeCollisionStateFix.Bootstrap.Transpile(new CodeInstruction[0]).ToList(), "Unknown collider IL rejected");
        Negative(() => KoeFreeIvaRefresh.Bootstrap.Transpile(new CodeInstruction[0]).ToList(), "Unknown FreeIVA IL rejected");
        Negative(() => KoeWaterfallRendererIndex.Bootstrap.Transpile(new CodeInstruction[0]).ToList(), "Unknown Waterfall IL rejected");
        var far = Resolve(null, new ResolveEventArgs("FerramAerospaceResearch"));
        var lookup = Method(far.GetType("ferram4.FARWingInteraction", true), "ExposureHitDetectionAndWingDetection");
        var lookupOld = PatchProcessor.GetOriginalInstructions(lookup);
        Check(
            KoeFarEventPath.ColliderOwners.Lookup(lookupOld, lookup).Count() == lookupOld.Count + 2,
            "FAR only narrows candidates at original enumeration"
        );
        Negative(() => KoeFarEventPath.ColliderOwners.Lookup(new CodeInstruction[0], lookup).ToList(), "Unknown FAR ownership IL rejected");
        var iva = Resolve(null, new ResolveEventArgs("FreeIva"));
        var enable = Method(iva.GetType("FreeIva.FreeIva", true), "EnableInternals");
        var enableOld = PatchProcessor.GetOriginalInstructions(enable);
        Check(KoeFreeIvaRefresh.ForeignRefresh.CreationParts(enableOld).Count() == enableOld.Count + 1, "Foreign refresh retains all cleanup calls");
        Negative(() => KoeFreeIvaRefresh.ForeignRefresh.CreationParts(new CodeInstruction[0])
            .ToList(), "Unknown FreeIVA creation IL rejected");
        var visit = Method(iva.GetType("FreeIva.InternalModuleFreeIva", true), "SetIsCurrentModule");
        var visitMethod = new DynamicMethod("RendererVisitValidation", typeof(void), Type.EmptyTypes);
        var visitGenerator = visitMethod.GetILGenerator();
        var visitOld = PatchProcessor.GetOriginalInstructions(visit, visitGenerator);
        var visitNew = KoeFreeIvaRefresh.RendererVisitBuffers.Transpile(visitOld, visitGenerator).ToList();
        Check(!visitNew.Any(c => c.opcode == OpCodes.Ldlen), "Pooled renderer capacity never used as traversal length");
        Check(
            visitNew.Count(c => c.operand is MethodInfo
            && ((MethodInfo)c.operand).Name == "get_material") == 1,
            "Material fetched once per current renderer"
        );
        Negative(
            () => KoeFreeIvaRefresh.RendererVisitBuffers.Transpile(new CodeInstruction[0], visitGenerator)
            .ToList(),
            "Unknown renderer visit rejected"
        );
        StagePool();
        LiveFuel();
        SortParity();
        if (Type.GetType("Mono.Runtime") != null)
            BindPatches();
        else
            Console.WriteLine("Player binding skipped: these Unity methods require the bundled Mono or KSP.");
    }

    static void StagePool()
    {
        var type = typeof(KoeEventPathFix.Bootstrap);
        var enter = Method(type, "EnterStage");
        var leave = Method(type, "LeaveStage");
        object[] outer =
        {
            0
        };
        enter.Invoke(null, outer);
        var first = KoeEventPathFix.Bootstrap.RentStage();
        first.Add(null);
        object[] nested =
        {
            0
        };
        enter.Invoke(null, nested);
        var second = KoeEventPathFix.Bootstrap.RentStage();
        second.Add(null);
        Check(!ReferenceEquals(first, second), "Nested stage activation uses separate list");
        var error = new InvalidOperationException("fixture");
        Check(ReferenceEquals(leave.Invoke(null, new[] { nested[0], (object)error }), error), "Stage exception unchanged");
        Check(second.Count == 0 && first.Count == 1, "Nested cleanup preserves parent");
        leave.Invoke(null, new[] { outer[0], null });
        Check(first.Count == 0, "Outer cleanup releases references");
    }

    static void LiveFuel()
    {
        var set = (PartSet)FormatterServices.GetUninitializedObject(typeof(PartSet));
        GC.SuppressFinalize(set);
        var resource = new PartResource((Part)null)
        {
            amount = 0,
            maxAmount = 20
        };
        var network = new PartSet.ResourcePrioritySet
        {
            lists = new List<List<PartResource>>
            {
                new List<PartResource>
                {
                    resource
                }
            },
            set = new HashSet<PartResource>
            {
                resource
            }
        };
        AccessTools.Field(typeof(PartSet), "pullList").SetValue(set, new Dictionary<int, PartSet.ResourcePrioritySet> { { 7, network } });
        var engine = (ModuleEngines)FormatterServices.GetUninitializedObject(typeof(ModuleEngines));
        var prop = new Propellant
        {
            ratio = 1,
            actualTotalAvailable = 0,
            displayName = "fixture fuel"
        };
        engine.propellants = new List<Propellant>
        {
            prop
        };
        engine.ignitionThreshold = 1;
        foreach (double next in new[]
        {
            0.0,
            10.0,
            0.0,
            5.0
        }

        )
        {
            resource.amount = next;
            double amount, capacity;
            set.GetConnectedResourceTotals(7, out amount, out capacity, true, false);
            Check(amount == next && capacity == 20, "Live totals observe direct refill without event");
            prop.actualTotalAvailable = amount;
            string name;
            bool deprived = engine.CheckDeprived(1, out name);
            Check(deprived == (next == 0) && prop.isDeprived == deprived, "Live stock engine deprivation reverses after refill");
        }
    }

    static void SortParity()
    {
        var assembly = Resolve(null, new ResolveEventArgs("FerramAerospaceResearch"));
        var type = assembly.GetType("ferram4.FARWingInteraction", true);
        var original = (Func<RaycastHit[], RaycastHit[]>)Delegate.CreateDelegate(typeof(Func<RaycastHit[], RaycastHit[]>), Method(type, "SortHitsByDistance"));
        var comparer = (IComparer<RaycastHit>)AccessTools.Field(type, "_comparer").GetValue(null);
        var random = new System.Random(8291);
        for (int round = 0; round < 400; round++)
        {
            var hits = new RaycastHit[round % 103];
            for (int i = 0; i < hits.Length; i++)
                hits[i] = new RaycastHit
                {
                    distance = round % 9 == 0 ? float.NaN : random.Next(-3, 6),
                    point = new Vector3(i, round, 0)
                };
            var expected = original(hits);
            var actual = KoeFarEventPath.Bootstrap.SortedCopy(hits, comparer);
            for (int i = 0; i < hits.Length; i++)
            {
                Check(hits[i].point.x == i, "Sort input unmodified");
                Check(expected[i].point == actual[i].point, "Exact tie/NaN ordering retained");
            }
        }
    }

    static void BindPatches()
    {
        // Exercise Harmony's actual method generation, including FAR's closed
        // generic ResetParts, without running game methods or native physics.
        foreach (string name in new[]
        {
            "SetupRenderer",
            "SetupSort",
            "SetupFairings"
        }

        )
        {
            var harmony = new Harmony("koe.test.events." + name);
            try
            {
                Method(typeof(KoeFarEventPath.Bootstrap), name).Invoke(null, new object[] { harmony });
                Check(true, "Actual FAR patch binding: " + name);
            }
            finally
            {
                harmony.UnpatchAll(harmony.Id);
            }
        }

        Console.WriteLine("Scene binding skipped: FAR and FreeIVA need FlightGlobals initialized by KSP.");
    }
}
