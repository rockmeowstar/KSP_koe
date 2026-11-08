using System;
using KerbalOverEngineering.Options;

static class LiveOptionsTests
{
    static void Check(bool condition) { if (!condition) throw new Exception("Live option test failed"); }
    static void Main()
    {
        bool native = true, value;
        LiveOptions.Register("test", true, enabled => { native = enabled; return true; });
        Check(LiveOptions.IsAvailable("test"));
        Check(LiveOptions.Apply("test", false) && !native);
        Check(LiveOptions.TryGet("test", out value) && !value);
        Check(LiveOptions.Apply("test", true) && native);
        LiveOptions.Register("failed", false, enabled => false);
        Check(!LiveOptions.Apply("failed", true));
        Check(LiveOptions.TryGet("failed", out value) && !value);
        LiveOptions.Register("throws", false, enabled => { throw new Exception(); });
        Check(!LiveOptions.Apply("throws", true));
        Check(LiveOptions.TryGet("throws", out value) && !value);
        LiveOptions.Unregister("test");
        Check(!LiveOptions.Apply("test", false) && !LiveOptions.TryGet("test", out value));
        Console.WriteLine("LIVE_OPTIONS_PASS");
    }
}
