using System;
using System.Collections;
using KerbalOverEngineering.Portraits;

static class LivePortraitCycleTests
{
    static void Check(bool value) { if (!value) throw new Exception("Portrait cycle test failed"); }
    static IEnumerator Source()
    {
        while (true) yield return "timed";
    }
    static void Main()
    {
        bool enabled = false, visible = true;
        var cycle = FrameCycle.Run(Source(), () => true, () => visible, wait => enabled && (string)wait == "timed");
        Check(cycle.MoveNext() && (string)cycle.Current == "timed");
        enabled = true;
        Check(cycle.MoveNext() && cycle.Current == null);
        visible = false;
        Check(cycle.MoveNext() && cycle.Current == null);
        visible = true;
        enabled = false;
        Check(cycle.MoveNext() && (string)cycle.Current == "timed");
        ((IDisposable)cycle).Dispose();
        Console.WriteLine("LIVE_PORTRAIT_CYCLE_PASS");
    }
}
