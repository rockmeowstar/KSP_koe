using System.Diagnostics;
using UnityEngine;

namespace KerbalOverEngineering.Options
{
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public sealed class RealtimePhysicsCatchup : MonoBehaviour
    {
        const string Option = "Stock.RealtimePhysicsCatchup";
        readonly Stopwatch clock = new Stopwatch();
        bool enabledOption;
        bool ownsBudget;
        bool externalOverride;
        float originalBudget;
        float writtenBudget;
        double previousTime;
        double completedTime;
        double debt;
        float sampledScale;
        float sampledStep;
        double referenceFrameSeconds;
        int referenceCap, referenceSync;

        public void Awake()
        {
            enabledOption = Settings.Enabled(Option);
            clock.Start();
            previousTime = clock.Elapsed.TotalSeconds;
            LiveOptions.Register(Option, enabledOption, Apply);
            GameEvents.onTimeWarpRateChanged.Add(WarpChanged);
        }

        void WarpChanged()
        {
            if (!enabledOption || externalOverride) return;
            RestoreBudget();
            previousTime = clock.Elapsed.TotalSeconds;
        }

        bool Apply(bool value)
        {
            RestoreBudget();
            enabledOption = value;
            externalOverride = false;
            previousTime = clock.Elapsed.TotalSeconds;
            return true;
        }

        void FixedUpdate()
        {
            if (enabledOption && ownsBudget && !externalOverride)
                completedTime += Time.fixedDeltaTime;
        }

        void LateUpdate()
        {
            double now = clock.Elapsed.TotalSeconds;
            double elapsed = now - previousTime;
            previousTime = now;

            if (!enabledOption || externalOverride) return;
            var vessel = FlightGlobals.ActiveVessel;
            bool physicalFlight = HighLogic.LoadedSceneIsFlight && FlightGlobals.ready
                && vessel != null && vessel.loaded && !vessel.packed && Time.timeScale > 0f
                && (TimeWarp.WarpMode == TimeWarp.Modes.LOW || TimeWarp.CurrentRate == 1f);
            if (!physicalFlight)
            {
                RestoreBudget();
                return;
            }

            if (ownsBudget && (sampledScale != Time.timeScale || sampledStep != Time.fixedDeltaTime))
            {
                RestoreBudget();
                return;
            }
            if (ownsBudget && Time.maximumDeltaTime != writtenBudget)
            {
                // Another component now controls the budget.
                ownsBudget = false;
                externalOverride = true;
                UnityEngine.Debug.Log("[KOE] RealtimePhysicsCatchup suspended after an external time-budget change.");
                return;
            }
            if (!ownsBudget)
            {
                originalBudget = Time.maximumDeltaTime;
                ownsBudget = true;
                // Skip the first sample after loading, packing, or unpausing.
                writtenBudget = originalBudget;
                sampledScale = Time.timeScale;
                sampledStep = Time.fixedDeltaTime;
                completedTime = 0;
                debt = 0;
                return;
            }

            float scale = Time.timeScale;
            float step = Time.fixedDeltaTime;
            if (sampledScale != scale || sampledStep != step)
            {
                // Start a new accounting interval when the player changes warp.
                RestoreBudget();
                return;
            }

            double target = elapsed * scale;
            if (double.IsNaN(target) || double.IsInfinity(target) || target <= 0 || step <= 0)
            {
                RestoreBudget();
                return;
            }
            bool precision = PhysicsWarpPrecision.IsActive;
            if (precision)
            {
                int cap = Application.targetFrameRate, sync = QualitySettings.vSyncCount;
                if (scale == 1)
                {
                    if (referenceCap != cap || referenceSync != sync) referenceFrameSeconds = 0;
                    referenceCap = cap; referenceSync = sync;
                    referenceFrameSeconds = referenceFrameSeconds > 0 ? System.Math.Min(referenceFrameSeconds, elapsed) : elapsed;
                }
                if (referenceFrameSeconds <= 0) referenceFrameSeconds = elapsed / scale;
                double interval = System.Math.Max(referenceFrameSeconds, cap > 0 ? 1.0 / cap : 0);
                // AI-generated comment: A slow accelerated physics batch must not enlarge the next batch.
                // Calibrate against 1x work and the render cap; excess debt cannot become an unbounded loop.
                target = System.Math.Min(elapsed, interval) * scale;
                debt = 0;
            }
            debt = System.Math.Max(0, debt + target - completedTime);
            completedTime = 0;

            // Recover at most one frame's worth of debt per rendered frame.
            // The remaining debt stays recorded instead of triggering a single long burst.
            double recovery = !precision && debt >= step ? System.Math.Min(debt, target) : 0;
            double steps = System.Math.Ceiling((target + recovery) / step);
            float required = Mathf.Max(originalBudget, (float)(steps * step));
            // Whole-step hysteresis avoids writing the Unity time budget every frame.
            if (Mathf.Abs(required - writtenBudget) < step) return;
            writtenBudget = required;
            Time.maximumDeltaTime = required;
            writtenBudget = Time.maximumDeltaTime;
        }

        void RestoreBudget()
        {
            if (ownsBudget && Time.maximumDeltaTime == writtenBudget)
                Time.maximumDeltaTime = originalBudget;
            ownsBudget = false;
            completedTime = 0;
            debt = 0;
        }

        public void OnDisable()
        {
            RestoreBudget();
            previousTime = clock.Elapsed.TotalSeconds;
        }

        public void OnDestroy()
        {
            RestoreBudget();
            LiveOptions.Unregister(Option);
            GameEvents.onTimeWarpRateChanged.Remove(WarpChanged);
            clock.Stop();
        }
    }
}
