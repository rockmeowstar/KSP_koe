using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using HarmonyLib;
using KoeDeltaVReadBatchFix;

static class DeltaVReadBatchTests
{
    static string gamePath;
    static int checks;
    const BindingFlags PrivateFields = BindingFlags.Instance | BindingFlags.NonPublic;
    static int Main(string[] args)
    {
        gamePath = args[0];
        AppDomain.CurrentDomain.AssemblyResolve += ResolveAssembly;
        try
        {
            CheckPatchLocations();
            CheckResourceSums();
            CheckPhaseBoundaries();
            CheckForeignPatchDetection();
            Console.WriteLine("Delta-V batching: " + checks + " checks passed against stock PartSet sums.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }

    static Assembly ResolveAssembly(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (string folder in new[]
        {
            "KSP_x64_Data/Managed",
            "GameData/000_Harmony"
        }

        )
        {
            string path = Path.Combine(gamePath, folder, name);
            if (File.Exists(path))
                return Assembly.Load(File.ReadAllBytes(path));
        }

        return null;
    }

    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition)
            throw new InvalidOperationException(message);
    }

    static PartSet CreateNetwork(double[] amounts)
    {
        var network = (PartSet)FormatterServices.GetUninitializedObject(typeof(PartSet));
        GC.SuppressFinalize(network);
        var resources = new List<PartResource>();
        foreach (double amount in amounts)
        {
            var resource = (PartResource)FormatterServices.GetUninitializedObject(typeof(PartResource));
            resource.amount = amount;
            resource.maxAmount = 100;
            resources.Add(resource);
        }

        int split = resources.Count / 2;
        var resourceList = new PartSet.ResourcePrioritySet
        {
            lists = new List<List<PartResource>>
            {
                resources.GetRange(0, split),
                resources.GetRange(split, resources.Count - split)
            },
            set = new HashSet<PartResource>(resources)
        };
        foreach (string name in new[]
        {
            "pullList",
            "pushList"
        }

        )
            typeof(PartSet).GetField(name, PrivateFields).SetValue(network, new Dictionary<int, PartSet.ResourcePrioritySet> { { 7, resourceList }, { 8, resourceList } });
        return network;
    }

    static Dictionary<int, PartSet.ResourcePrioritySet> PullLists(PartSet network)
    {
        return (Dictionary<int, PartSet.ResourcePrioritySet>)typeof(PartSet).GetField("pullList", PrivateFields).GetValue(network);
    }

    static void CheckStockSum(PartSet network, int resourceId, bool pulling, string scenario)
    {
        double expectedAmount, expectedCapacity, amount, capacity;
        network.GetConnectedResourceTotals(resourceId, out expectedAmount, out expectedCapacity, pulling, true);
        Check(
            ReadBatch.TryRead(network, resourceId, pulling, true, out amount, out capacity),
            scenario
            + ": the initialized simulation network should be batched."
        );
        CheckBits(amount, expectedAmount, scenario + ": amount");
        CheckBits(capacity, expectedCapacity, scenario + ": capacity");
    }

    static void CheckBits(double actual, double expected, string scenario)
    {
        long actualBits = BitConverter.DoubleToInt64Bits(actual);
        long expectedBits = BitConverter.DoubleToInt64Bits(expected);
        Check(actualBits == expectedBits, scenario
            + ": expected bits " + expectedBits.ToString("X16")
            + ", got " + actualBits.ToString("X16")
            + ".");
    }

    static void CheckResourceSums()
    {
        var random = new Random(4321);
        for (int sample = 0; sample < 300; sample++)
        {
            var amounts = new double[random.Next(0, 129)];
            for (int i = 0; i < amounts.Length; i++)
                amounts[i] = sample % 13 == 0 ? BitConverter.Int64BitsToDouble(unchecked((long)0x7ff8000000000001) + i) : Math.Pow(10, random.Next(-200, 200)) * random.NextDouble();
            var network = CreateNetwork(amounts);
            using (var batch = new ReadBatch())
            {
                foreach (bool pulling in new[]
                {
                    true,
                    false
                }

                )
                    foreach (int resourceId in new[]
                    {
                        7,
                        8
                    }

                    )
                    {
                        string scenario = "Sample " + sample + ", resource " + resourceId + (pulling ? ", pulling" : ", pushing");
                        CheckStockSum(network, resourceId, pulling, scenario + ", first read");
                        CheckStockSum(network, resourceId, pulling, scenario + ", cached read");
                    }
            }
        }

        using (var batch = new ReadBatch())
        {
            foreach (var values in new[]
            {
                new[]
                {
                    0.0,
                    -0.0
                },
                new[]
                {
                    double.PositiveInfinity,
                    double.NegativeInfinity
                },
                new[]
                {
                    1e200,
                    -1e200,
                    1e-200
                }
            }

            )
            {
                var network = CreateNetwork(values);
                CheckStockSum(network, 7, true, "Special floating-point values");
                CheckStockSum(network, 7, true, "Cached special floating-point values");
            }
        }
    }

    static void CheckPhaseBoundaries()
    {
        var network = CreateNetwork(new[] { 1.0 });
        double amount, capacity;
        Check(!ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "A query outside SimulateDeltaV must use KSP.");
        using (var batch = new ReadBatch())
        {
            CheckStockSum(network, 7, true, "Initial resource read");
            typeof(PartSet).GetField("simulationSet", PrivateFields).SetValue(network, false);
            Check(ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "A ready network should have a cache hit.");
            Check(
                (bool)typeof(PartSet).GetField("simulationSet", PrivateFields).GetValue(network),
                "A cache hit must set PartSet.simulationSet to true, as KSP does."
            );
            PullLists(network)[7].lists[1][0].amount = 17;
            ReadBatch.Barrier();
            CheckStockSum(network, 7, true, "Direct refill after a phase boundary");
            batch.Suppress();
            Check(!ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "An engine with mod callbacks must use KSP.");
            using (var nested = new ReadBatch())
                CheckStockSum(network, 7, true, "Independent nested simulation");
            Check(!ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "The nested simulation must not cancel its parent's suppression.");
            batch.Resume();
            CheckStockSum(network, 7, true, "Parent simulation resumes");
            using (var nested = new ReadBatch())
                PullLists(network)[7].lists[1][0].amount = 29;
            CheckStockSum(network, 7, true, "Parent observes fuel changed by a nested simulation");
            Check(!ReadBatch.TryRead(network, 7, true, false, out amount, out capacity), "Real fuel queries must not use simulated totals.");
            Check(!ReadBatch.TryRead(null, 7, true, true, out amount, out capacity), "A missing network must use KSP.");
            var other = CreateNetwork(new[] { 43.0 });
            CheckStockSum(other, 7, true, "A different network with the same resource ID");
            PullLists(network).Remove(7);
            Check(!ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "KSP must create a missing resource list itself.");
            PullLists(network)[7] = PullLists(other)[7];
            CheckStockSum(network, 7, true, "A replacement resource list");
        }

        try
        {
            using (var batch = new ReadBatch())
                throw new ArgumentException("Test exception");
        }
        catch (ArgumentException)
        {
        }

        Check(!ReadBatch.TryRead(network, 7, true, true, out amount, out capacity), "An exception must leave no active batch.");
    }

    static void CheckPatchLocations()
    {
        var routing = PatchProcessor.GetOriginalInstructions(AccessTools.Method(typeof(Bootstrap), "SimulationNetwork"));
        Check(
            routing.Any(code => code.Calls(AccessTools.Method(typeof(UnityEngine.Object), "op_Equality"))),
            "Vessel-wide routing must use Unity's null check so a destroyed vessel cannot return stale totals."
        );
        var simulation = PatchProcessor.GetOriginalInstructions(AccessTools.Method(typeof(DeltaVStageInfo), "SimulateDeltaV"));
        var patched = Bootstrap.BatchPhases(simulation).ToList();
        for (int i = 0; i < patched.Count; i++)
        {
            var method = patched[i].operand as MethodInfo;
            if (method != null && method.DeclaringType == typeof(DeltaVEngineInfo) && method.Name == "ApplyBurn")
                Check(
                    i > 0
                    && patched[i - 1].Calls(AccessTools.Method(typeof(ReadBatch), "Barrier")),
                    "SimulateDeltaV must clear cached totals immediately before ApplyBurn changes fuel."
                );
        }

        var burn = PatchProcessor.GetOriginalInstructions(AccessTools.Method(typeof(DeltaVEngineInfo), "CalculateBurn"));
        Check(Bootstrap.BatchQuery(burn).Count() == burn.Count, "The CalculateBurn patch must replace one call without changing the instruction count.");
        CheckRejected(() => Bootstrap.BatchPhases(new CodeInstruction[0]).ToList(), "An unknown SimulateDeltaV body");
        CheckRejected(() => Bootstrap.BatchQuery(new CodeInstruction[0]).ToList(), "An unknown CalculateBurn body");
    }

    static void CheckRejected(Action action, string scenario)
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

        Check(rejected, scenario + " must leave the original implementation in place.");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int ForeignTarget(int value)
    {
        return value + 1;
    }

    static void ForeignPrefix()
    {
    }

    static void CheckForeignPatchDetection()
    {
        var watched = AccessTools.Field(typeof(Bootstrap), "watchedMethods");
        var detect = AccessTools.Method(typeof(Bootstrap), "HasForeignPatch");
        watched.SetValue(null, new[] { AccessTools.Method(typeof(DeltaVReadBatchTests), "ForeignTarget") });
        var harmony = new Harmony("koe.tests.foreign-delta-v-patch");
        try
        {
            Check(!(bool)detect.Invoke(null, null), "An unpatched method should allow batching.");
            harmony.Patch(
                AccessTools.Method(typeof(DeltaVReadBatchTests), "ForeignTarget"),
                prefix: new HarmonyMethod(typeof(DeltaVReadBatchTests), "ForeignPrefix")
            );
            Check((bool)detect.Invoke(null, null), "A foreign Harmony prefix must disable batching for that simulation.");
        }
        finally
        {
            harmony.UnpatchAll(harmony.Id);
            watched.SetValue(null, null);
        }
    }
}
