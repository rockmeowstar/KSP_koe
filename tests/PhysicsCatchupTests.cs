using System;
using System.Reflection;
using KerbalOverEngineering.Options;

namespace UnityEngine
{
    public class MonoBehaviour { }
    public static class Application { public static int targetFrameRate = 120; }
    public struct Resolution { public int refreshRate; }
    public static class Screen { public static Resolution currentResolution = new Resolution { refreshRate = 75 }; }
    public static class QualitySettings { public static int vSyncCount; }
    public static class Time { public static float timeScale = 1, fixedDeltaTime = .02f, maximumDeltaTime = .04f; public static bool inFixedTimeStep; public static int frameCount; }
    public static class Mathf
    {
        public static float Max(float a, float b) { return Math.Max(a, b); }
        public static float Abs(float a) { return Math.Abs(a); }
    }
    public static class Debug { public static void Log(string value) { } }
}
public class KSPAddon : Attribute
{
    public enum Startup { Flight }
    public KSPAddon(Startup startup, bool persistent) { }
}
public class Vessel { public bool loaded = true, packed; }
public static class HighLogic { public static bool LoadedSceneIsFlight = true; }
public class Planetarium
{
    public static Planetarium fetch = new Planetarium();
    public static double TimeScale = 1;
    public double fixedDeltaTime = .02;
}
public static class FlightGlobals { public static bool ready = true; public static Vessel ActiveVessel = new Vessel(); }
public static class GameEvents
{
    public sealed class Event
    {
        Action handlers;
        public void Add(Action handler) { handlers += handler; }
        public void Remove(Action handler) { handlers -= handler; }
        public void Fire() { if (handlers != null) handlers(); }
    }
    public static readonly Event onTimeWarpRateChanged = new Event();
}
public class TimeWarp
{
    public static TimeWarp fetch = new TimeWarp();
    bool setMode(Modes mode) { WarpMode = mode; return true; }
    public enum Modes { LOW, HIGH }
    public static Modes WarpMode = Modes.LOW;
    public static float CurrentRate = 1;
    public static int LastRate = -1;
    public static bool LastInstant, LastMessage;
    public static void SetRate(int rate, bool instant, bool message) { LastRate = rate; LastInstant = instant; LastMessage = message; }
}
namespace KerbalOverEngineering.Options
{
    public static class Settings { public static bool Enabled(string key) { return true; } }
    public static class LiveOptions
    {
        public static void Register(string key, bool value, Func<bool,bool> action) { }
        public static void Unregister(string key) { }
    }
}

static class PhysicsCatchupTests
{
    static int checks;
    static void Check(bool condition, string name)
    {
        checks++;
        if (!condition) throw new Exception(name);
    }
    static object Field(object instance, string name)
    {
        return instance.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(instance);
    }
    static void Invoke(object instance, string name)
    {
        instance.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic).Invoke(instance, null);
    }
    static void Frame(RealtimePhysicsCatchup target, double seconds, int steps)
    {
        for (int i=0;i<steps;i++) Invoke(target,"FixedUpdate");
        var timer=(System.Diagnostics.Stopwatch)Field(target,"clock");
        target.GetType().GetField("previousTime",BindingFlags.Instance|BindingFlags.NonPublic)
            .SetValue(target,timer.Elapsed.TotalSeconds-seconds);
        Invoke(target,"LateUpdate");
    }
    public static void Main()
    {
        foreach (var sample in new[] { new[] { 6.25, 6.25 },
            new[] { 12.5, 25.0 }, new[] { 25.0, 50.0 }, new[] { 60.0, 50.0 } })
            Check(Math.Abs(RealtimePhysicsCatchup.TargetPhysicsRate(sample[0]) - sample[1]) < 1e-9, "physics rate curve");
        double previous = 6.25;
        for (int i = 1; i <= 750; i++)
        {
            double fps = 6.25 + i * .025;
            double rate = RealtimePhysicsCatchup.TargetPhysicsRate(fps);
            Check(rate >= previous - 1e-9 && rate >= fps - 1e-9 && rate <= 50 + 1e-9, "curve monotonicity");
            previous = rate;
        }
        Check(Math.Abs(RealtimePhysicsCatchup.TargetPhysicsRate(15.625) - 45) < 1e-9,
            "upper segment misses the 90 percent target");
        foreach (double point in new[] { 6.25, 12.5 })
        {
            double h = .0001;
            double left = (RealtimePhysicsCatchup.TargetPhysicsRate(point) - RealtimePhysicsCatchup.TargetPhysicsRate(point - h)) / h;
            double right = (RealtimePhysicsCatchup.TargetPhysicsRate(point + h) - RealtimePhysicsCatchup.TargetPhysicsRate(point)) / h;
            Check(Math.Abs(left - right) < .001, "curve slope discontinuity");
        }
        double credit = 1, allocated = 0;
        for (int i = 0; i < 40; i++) allocated += RealtimePhysicsCatchup.AllocateSteps(ref credit, 1.75);
        Check(allocated == 71, "fractional step distribution or initial lead");
        credit = 1;
        Check(RealtimePhysicsCatchup.FrameStepLimit(6.25, 1, ref credit) == 1 && credit == 0,
            "low-fps limit retained the extra step");
        Check(RealtimePhysicsCatchup.FrameStepLimit(3, 4, ref credit) == 1,
            "low-fps limit allowed multiple steps");
        foreach (int fps in new[] { 5,10,60 })
        {
            UnityEngine.Time.maximumDeltaTime=.04f;
            UnityEngine.Time.timeScale=1;
            UnityEngine.Time.fixedDeltaTime=.02f;
            var target=new RealtimePhysicsCatchup();
            target.Awake();
            Frame(target,1.0/fps,0);
            foreach (int warp in new[] {3,1,2,4,1})
            {
                UnityEngine.Time.timeScale=warp;
                UnityEngine.Time.fixedDeltaTime=.02f*warp;
                // Simulate Unity clamping the maximum budget during a warp change.
                UnityEngine.Time.maximumDeltaTime=Math.Max(.04f,UnityEngine.Time.fixedDeltaTime);
                Frame(target,1.0/fps,1);
                Check(!(bool)Field(target,"externalOverride"),"warp incorrectly classified as external override");
                Check((double)Field(target,"debt")==0,"old warp debt retained");
                Frame(target,1.0/fps,0);
                Frame(target,1.0/fps,1);
                Check(UnityEngine.Time.maximumDeltaTime>=UnityEngine.Time.fixedDeltaTime,"budget smaller than step");
                Check(UnityEngine.Time.fixedDeltaTime==.02f*warp,"fixed step modified");
                Check(UnityEngine.Time.timeScale==warp,"warp modified");
                Check((double)Field(target,"debt")>=0,"negative debt");
                target.OnDisable();
                Check(!(bool)Field(target,"ownsBudget"),"budget ownership leaked");
            }
            Frame(target,1.0/fps,0);
            UnityEngine.Time.timeScale=0;
            Frame(target,1.0/fps,0);
            Check(!(bool)Field(target,"ownsBudget"),"pause retained budget ownership");
            Check((double)Field(target,"debt")==0,"pause retained time debt");
            UnityEngine.Time.timeScale=1;
            Frame(target,1.0/fps,0);
            FlightGlobals.ActiveVessel.packed=true;
            Frame(target,1.0/fps,0);
            Check(!(bool)Field(target,"ownsBudget"),"packed vessel retained budget ownership");
            FlightGlobals.ActiveVessel.packed=false;
            Frame(target,1.0/fps,0);
            Frame(target,1.0/fps,0);
            GameEvents.onTimeWarpRateChanged.Fire();
            UnityEngine.Time.maximumDeltaTime=.04f;
            Frame(target,1.0/fps,0);
            Check(!(bool)Field(target,"externalOverride"),"same-rate warp event caused suspension");
            Check((double)Field(target,"debt")==0,"same-rate warp event retained debt");
            Frame(target,1.0/fps,0);
            UnityEngine.Time.maximumDeltaTime=.31f;
            Frame(target,1.0/fps,1);
            Check((bool)Field(target,"externalOverride"),"external override ignored");
            target.OnDestroy();
            Check(UnityEngine.Time.maximumDeltaTime==.31f,"foreign budget overwritten");
        }
        UnityEngine.Time.timeScale=1;
        UnityEngine.Time.fixedDeltaTime=.02f;
        TimeWarp.WarpMode=TimeWarp.Modes.LOW;
        var unsupported=new PhysicsWarpPrecision();
        unsupported.Awake();
        Check(!PhysicsWarpPrecision.IsActive,"precision enabled without its patches");
        unsupported.OnDestroy();
        PhysicsWarpPrecision.PatchesReady=true;
        var precision=new PhysicsWarpPrecision();
        precision.Awake();
        UnityEngine.Time.inFixedTimeStep=true;
        Check(PhysicsWarpPrecision.DeferWarp(3,true,false),"in-step warp was not deferred");
        Check(TimeWarp.LastRate==-1,"warp ran inside the fixed step");
        Check(PhysicsWarpPrecision.DeferWarp(2,false,true),"replacement warp was not deferred");
        TimeWarp.WarpMode=TimeWarp.Modes.HIGH;
        UnityEngine.Time.inFixedTimeStep=false;
        Invoke(precision,"LateUpdate");
        Check(TimeWarp.WarpMode==TimeWarp.Modes.LOW,"deferred physical warp became on-rails warp");
        Check(TimeWarp.LastRate==2&&!TimeWarp.LastInstant&&TimeWarp.LastMessage,"deferred warp arguments changed");
        Check(!PhysicsWarpPrecision.DeferWarp(4,true,true),"render-frame warp was unnecessarily delayed");
        UnityEngine.Application.targetFrameRate=5;
        var bounded=new RealtimePhysicsCatchup();
        bounded.Awake();
        Frame(bounded,.2,0); Frame(bounded,.2,10);
        UnityEngine.Time.timeScale=4;
        Planetarium.TimeScale=4;
        GameEvents.onTimeWarpRateChanged.Fire();
        Frame(bounded,.2,0);
        Frame(bounded,8,40);
        Check(UnityEngine.Time.maximumDeltaTime<.83f,"slow precision batch enlarged the next time budget");
        Check(UnityEngine.Time.fixedDeltaTime==.02f,"bounded catch-up changed precision step");
        bounded.OnDestroy();
        UnityEngine.Time.timeScale=1;
        Planetarium.TimeScale=1;
        GameEvents.onTimeWarpRateChanged.Fire();
        foreach(int warp in new[] {3,1,2,4,1})
        {
            UnityEngine.Time.timeScale=warp;
            Planetarium.TimeScale=warp;
            UnityEngine.Time.fixedDeltaTime=.02f*warp;
            GameEvents.onTimeWarpRateChanged.Fire();
            Check(UnityEngine.Time.fixedDeltaTime==.02f,"precision changed base step");
            Check(UnityEngine.Time.timeScale==warp,"precision changed warp");
            Check(Math.Abs(Planetarium.fetch.fixedDeltaTime*Planetarium.TimeScale-.02)<1e-8,"planetarium and solver steps differ");
        }
        precision.OnDisable();
        Check(UnityEngine.Time.fixedDeltaTime==.02f,"precision restored wrong 1x step");
        precision.OnDestroy();
        Check(!PhysicsWarpPrecision.IsActive,"destroyed flight retained precision state");
        UnityEngine.Time.timeScale=4;
        Planetarium.TimeScale=4;
        UnityEngine.Time.fixedDeltaTime=.08f;
        precision=new PhysicsWarpPrecision();
        precision.Awake();
        Check(UnityEngine.Time.fixedDeltaTime==.02f,"precision enabled at 4x incorrectly");
        precision.OnDisable();
        Check(UnityEngine.Time.fixedDeltaTime==.08f,"precision did not restore 4x");
        Check(Planetarium.fetch.fixedDeltaTime==.02,"precision did not restore planetarium clock");
        precision.OnDestroy();
        UnityEngine.Time.timeScale=1;
        Planetarium.TimeScale=1;
        UnityEngine.Time.fixedDeltaTime=.02f;
        TimeWarp.WarpMode=TimeWarp.Modes.HIGH;
        TimeWarp.CurrentRate=1;
        precision=new PhysicsWarpPrecision();
        precision.Awake();
        Check(PhysicsWarpPrecision.IsActive,"normal-rate high mode must use the same control loop");
        TimeWarp.CurrentRate=10;
        Planetarium.TimeScale=10;
        GameEvents.onTimeWarpRateChanged.Fire();
        Check(!PhysicsWarpPrecision.IsActive,"on-rails warp entered precision mode");
        Check(Planetarium.fetch.fixedDeltaTime==.02,"on-rails transition retained the modified clock");
        precision.OnDestroy();
        TimeWarp.CurrentRate=1;
        Planetarium.TimeScale=1;
        precision=new PhysicsWarpPrecision();
        precision.Awake();
        UnityEngine.Time.fixedDeltaTime=.03f;
        Check(!PhysicsWarpPrecision.IsActive,"external step still uses precision integration rules");
        Invoke(precision,"LateUpdate");
        Check(UnityEngine.Time.fixedDeltaTime==.03f,"precision overwrote an external step");
        precision.OnDestroy();
        Console.WriteLine("PHYSICS_CATCHUP_CHECKS_PASSED "+checks);
    }
}
