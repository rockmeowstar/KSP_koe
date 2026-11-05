using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using HarmonyLib;
using PqsNoiseFix;

static class PqsKernelTests
{
    static string game;
    static int checks;
    static void Check(bool value, string message)
    {
        checks++;
        if (!value)
            throw new Exception(message);
    }

    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name + ".dll";
        foreach (string folder in new[]
        {
            @"KSP_x64_Data\Managed",
            @"GameData\000_Harmony"
        }

        )
        {
            string path = Path.Combine(game, folder, name);
            if (File.Exists(path))
                return Assembly.Load(File.ReadAllBytes(path));
        }

        return null;
    }

    static int Main(string[] args)
    {
        try
        {
            game = args[0];
            AppDomain.CurrentDomain.AssemblyResolve += Resolve;
            Run();
            Console.WriteLine("PQS kernels: " + checks + " IL checks passed.");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        // AI-generated comment: A planet with a custom terrain pipeline need not contain any of our
        // admitted noise generators. It must remain a quiet no-op, not throw.
        var sphere = (PQS)FormatterServices.GetUninitializedObject(typeof(PQS));
        var batch = new NoiseKernel.Batch();
        var current = typeof(NoiseKernel).GetField("current", BindingFlags.Static | BindingFlags.NonPublic);
        current.SetValue(null, batch);
        try
        {
            NoiseKernel.Prepare(sphere);
            Check(!batch.Enabled && !batch.Ready, "Custom terrain without supported generators stays inactive");
        }
        finally
        {
            current.SetValue(null, null);
        }

        MethodInfo init;
        MethodInfo[] callbacks;
        NoiseKernel.Bind(out init, out callbacks);
        Check(callbacks.Length == 5, "Five admitted callbacks");
        foreach (var method in callbacks)
        {
            var original = PatchProcessor.GetOriginalInstructions(method).ToList();
            var changed = NoiseKernel.Callbacks(original, method).ToList();
            int edits = 0;
            Check(original.Count == changed.Count, "Callback instruction count preserved");
            for (int i = 0; i < original.Count; i++)
            {
                Check(
                    original[i].labels.SequenceEqual(changed[i].labels)
                    && original[i].blocks.SequenceEqual(changed[i].blocks),
                    "Callback control flow preserved"
                );
                if (original[i].opcode != changed[i].opcode || !Equals(original[i].operand, changed[i].operand))
                    edits++;
            }

            Check(
                edits == (method.DeclaringType == typeof(PQSLandControl)
                && method.Name == "OnVertexBuildHeight" ? 4 : 1),
                "Only expected noise calls changed"
            );
        }

        var before = PatchProcessor.GetOriginalInstructions(init).ToList();
        var after = NoiseKernel.Init(before).ToList();
        Check(after.Count == before.Count + 2, "Only preparation call inserted");
        int position = 0, insertions = 0;
        foreach (var instruction in before)
        {
            var changed = after[position++];
            Check(
                instruction.opcode == changed.opcode
                && Equals(instruction.operand, changed.operand)
                && instruction.labels.SequenceEqual(changed.labels)
                && instruction.blocks.SequenceEqual(changed.blocks),
                "Quad initialization instruction preserved"
            );
            if (position + 1 < after.Count
                && after[position].opcode == OpCodes.Ldarg_0
                && after[position + 1].Calls(AccessTools.Method(typeof(NoiseKernel), "Prepare")))
            {
                position += 2;
                insertions++;
            }
        }

        Check(position == after.Count && insertions == 1, "Exactly one build boundary");
        bool rejected = false;
        try
        {
            NoiseKernel.Init(new CodeInstruction[0]).ToList();
        }
        catch (NotSupportedException)
        {
            rejected = true;
        }

        Check(rejected, "Unknown initialization rejected");
        rejected = false;
        try
        {
            NoiseKernel.Callbacks(new CodeInstruction[0], callbacks[0]).ToList();
        }
        catch (NotSupportedException)
        {
            rejected = true;
        }

        Check(rejected, "Unknown callback rejected");
    }
}
