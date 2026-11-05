using System;
using KerbalOverEngineering.Options;

static class LoadingResourcesTests
{
    static int checks;
    static void Check(bool ok, string text)
    {
        checks++;
        if (!ok)
            throw new Exception(text);
    }

    static int Main()
    {
        try
        {
            foreach (int cores in new[]
            {
                1,
                4,
                20,
                64,
                128
            }

            )
                foreach (long memory in new[]
                {
                    4L << 30,
                    16L << 30,
                    64L << 30,
                    256L << 30
                }

                )
                    foreach (long free in new[]
                    {
                        0L,
                        memory / 64,
                        memory / 2,
                        memory
                    }

                    )
                    {
                        var policy = new LoadingResources(cores, memory, free);
                        long sum = policy.DdsBudget + policy.EveBudget + policy.PngBudget + policy.BiomeBudget + policy.NoiseBudget;
                        Check(sum <= free / 8 && sum <= memory / 16, "Aggregate queues respect available and installed physical memory");
                        Check(policy.Workers >= 1 && policy.Workers <= cores, "Worker count respects actual logical processors");
                        Check(policy.DecodeWindow == policy.Workers, "Decode concurrency follows processor and memory limits");
                        Check(
                            policy.NoiseCoordinateLimit >= 0
                            && policy.NoiseCoordinateLimit * 128L <= policy.NoiseBudget / 2,
                            "Noise coordinate limit respects detected memory budget"
                        );
                        if (free == 0)
                            Check(sum == 0, "Unknown/unavailable RAM disables speculative allocation");
                    }

            var small = new LoadingResources(4, 8L << 30, 2L << 30);
            var large = new LoadingResources(20, 64L << 30, 32L << 30);
            Check(large.Workers > small.Workers && large.PngBudget > small.PngBudget, "Larger hardware receives more workers and budget");
            var detected = LoadingResources.Current;
            Check(LoadingResources.FrameBudget(.001f, 120, 75) == 1f / 120, "Loading budget follows requested frame rate");
            Check(LoadingResources.FrameBudget(.001f, -1, 144) == 1f / 144, "Uncapped loading follows display rate");
            Check(LoadingResources.FrameBudget(.02f, 120, 75) == .02f, "Keep a larger stock loading budget");
            Check(LoadingResources.FrameBudget(.001f, -1, 0) == .001f, "Unknown rates preserve stock budget");
            Console.WriteLine("Loading budgets: " + checks + " checks passed; detected workers: " + detected.Workers + ".");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
