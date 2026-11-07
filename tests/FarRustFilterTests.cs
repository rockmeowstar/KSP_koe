using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using HarmonyLib;
using KoeFarEventPath;
using UnityEngine;

static class FarRustFilterTests
{
    [StructLayout(LayoutKind.Sequential)] struct Owner { public long Padding; public int Layer; }
    [StructLayout(LayoutKind.Sequential)] struct Component { public long Padding1, Padding2; public IntPtr Owner; }
    static string game;
    static int checks;
    static FieldInfo nativePointer;
    static readonly List<IntPtr> allocations = new List<IntPtr>();

    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new Exception(message);
    }
    static IntPtr Allocate<T>(T value) where T : struct
    {
        var memory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(T)));
        allocations.Add(memory);
        Marshal.StructureToPtr(value, memory, false);
        return memory;
    }
    static Renderer Renderer(int layer, bool missingOwner = false)
    {
        var owner = Allocate(new Owner { Layer = layer });
        var component = Allocate(new Component { Owner = missingOwner ? IntPtr.Zero : owner });
        var renderer = (MeshRenderer)FormatterServices.GetUninitializedObject(typeof(MeshRenderer));
        GC.SuppressFinalize(renderer);
        nativePointer.SetValue(renderer, component);
        return renderer;
    }
    static bool Excluded(Renderer renderer)
    {
        if (ReferenceEquals(renderer, null)) return false;
        var component = (IntPtr)nativePointer.GetValue(renderer);
        var owner = Marshal.ReadIntPtr(component, (int)Marshal.OffsetOf(typeof(Component), "Owner"));
        return owner != IntPtr.Zero && Marshal.ReadInt32(owner, (int)Marshal.OffsetOf(typeof(Owner), "Layer")) != 0;
    }
    static void Compare(List<Renderer> source, Predicate<Renderer> predicate)
    {
        var expected = new List<Renderer>(source);
        var actual = new List<Renderer>(source);
        Check(expected.RemoveAll(predicate) == RustRendererFilter.Filter(actual, predicate), "Removed count must match List.RemoveAll");
        Check(actual.Count == expected.Count, "Survivor count must match");
        for (int i = 0; i < actual.Count; i++) Check(ReferenceEquals(actual[i], expected[i]), "Survivor identity and order must match");
    }
    static int Main(string[] args)
    {
        game = args[0];
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
        try
        {
            Run(args[1]);
            Console.WriteLine("FAR Rust filter: " + checks + " bridge/kernel/IL checks passed");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally { foreach (var memory in allocations) Marshal.FreeHGlobal(memory); }
    }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (var directory in new[] { game + "/KSP_x64_Data/Managed", game + "/GameData/000_Harmony", game + "/GameData/FerramAerospaceResearch/Plugins", game + "/GameData/000_KSPBurst/Plugins" })
        {
            string path = Path.Combine(directory, name);
            if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
        }
        return null;
    }
    static void Run(string library)
    {
        nativePointer = AccessTools.Field(typeof(UnityEngine.Object), "m_CachedPtr");
        RustRendererFilter.LoadKernel(library);
        RustRendererFilter.OwnerOffset = (uint)(int)Marshal.OffsetOf(typeof(Component), "Owner");
        RustRendererFilter.LayerOffset = (uint)(int)Marshal.OffsetOf(typeof(Owner), "Layer");
        var values = Enumerable.Range(0, 32).Select(layer => Renderer(layer)).ToArray();
        var random = new System.Random(823);
        for (int trial = 0; trial < 200; trial++)
        {
            var source = new List<Renderer>();
            for (int i = 0; i < trial % 48; i++) source.Add(random.Next(5) == 0 ? null : values[random.Next(values.Length)]);
            Compare(source, Excluded);
        }
        long fallbacks = RustRendererFilter.Fallbacks;
        Compare(new List<Renderer> { values[0], Renderer(3, true), values[31] }, Excluded);
        Compare(new List<Renderer> { values[0], Renderer(32), values[31] }, Excluded);
        Check(RustRendererFilter.Fallbacks == fallbacks + 2, "Invalid native input uses the original predicate before changing the list");
        var nested = new List<Renderer> { values[0], values[31], null };
        var invalid = Renderer(32);
        bool entered = false;
        Predicate<Renderer> reentrant = item => {
            if (!entered) { entered = true; Compare(nested, Excluded); }
            return Excluded(item);
        };
        Compare(new List<Renderer> { values[0], invalid, values[31] }, reentrant);
        Check(entered, "Fallback can reenter filtering without reusing an active workspace");
        long calls = RustRendererFilter.Calls;
        var arbitrary = new List<Renderer> { values[0], values[31] };
        Check(RustRendererFilter.Remove(arbitrary, _ => true) == 2, "An unrecognized predicate retains its actual behavior");
        Check(RustRendererFilter.Calls == calls, "Unrecognized predicates do not enter the native kernel");
        var far = Assembly.Load(File.ReadAllBytes(game + "/GameData/FerramAerospaceResearch/Plugins/FerramAerospaceResearch.dll"));
        var reset = AccessTools.Method(far.GetType("FerramAerospaceResearch.FARAeroComponents.VehicleExposure"), "ResetParts").MakeGenericMethod(typeof(List<Part>));
        var original = PatchProcessor.GetOriginalInstructions(reset).ToList();
        var changed = RustRendererFilter.Rewrite(original).ToList();
        Check(changed.Count == original.Count, "Instruction count remains unchanged");
        Check(changed.Count(x => x.Calls(AccessTools.Method(typeof(RustRendererFilter), "Remove"))) == 1, "Exactly one filter call is replaced");
        Check(original.Zip(changed, (a, b) => a.opcode != b.opcode || !Equals(a.operand, b.operand)).Count(x => x) == 1, "Other reset steps retain their original IL");
        var setup = original.Select(x => x.operand as MethodInfo).Single(x => x != null && x.Name == "SetupRenderers");
        var rendererComparer = RustRendererSetup.DefaultComparer<Renderer>(setup.DeclaringType.Assembly);
        var partComparer = RustRendererSetup.DefaultComparer<Part>(setup.DeclaringType.Assembly);
        var rendererPool = AccessTools.Field(setup.DeclaringType, "renderersPool").GetValue(null);
        var pooled = (HashSet<Renderer>)AccessTools.Method(rendererPool.GetType(), "Acquire").Invoke(rendererPool, null);
        Check(ReferenceEquals(pooled.Comparer, rendererComparer), "Native batching recognizes the comparer used by the actual FAR renderer pool");
        Check(ReferenceEquals(partComparer, RustRendererSetup.DefaultComparer<Part>(setup.DeclaringType.Assembly)), "FAR part comparer singleton is retained");
        AccessTools.Method(rendererPool.GetType(), "Release").Invoke(rendererPool, new object[] { pooled });
        AccessTools.Field(typeof(RustRendererSetup), "setupMethod").SetValue(null, setup);
        var propertyCalls = RustRendererSetup.Rewrite(original).ToList();
        Check(propertyCalls.Count == original.Count, "Property batch changes no instruction positions");
        Check(propertyCalls.Count(x => x.Calls(AccessTools.Method(typeof(RustRendererSetup), "Setup"))) == 1, "One setup callsite is adapted");
        Check(original.Zip(propertyCalls, (a, b) => a.opcode != b.opcode || !Equals(a.operand, b.operand)).Count(x => x) == 1, "Reset, filtering and other original steps stay intact");
    }
}
