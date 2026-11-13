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
        double stepCredit;
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
                stepCredit = 1;
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
                if (scale > 1f)
                {
                    if (referenceFrameSeconds <= 0) referenceFrameSeconds = elapsed / scale;
                    double interval = System.Math.Max(referenceFrameSeconds, cap > 0 ? 1.0 / cap : 0);
                    // AI-generated comment: Bound accelerated batches by the observed 1x frame interval.
                    target = System.Math.Min(elapsed, interval) * scale;
                    debt = 0;
                }
            }
            debt = System.Math.Max(0, debt + target - completedTime);
            completedTime = 0;

            double fps = 1.0 / elapsed;
            double desiredSteps = TargetPhysicsRate(fps) * elapsed * scale;
            if (precision && scale > 1f) desiredSteps = System.Math.Min(desiredSteps, target / step);
            // Above 25 FPS the cap must allow Unity to consume its normal 50 Hz clock.
            double steps = FrameStepLimit(fps, desiredSteps, ref stepCredit);
            float required = Mathf.Max(step, (float)(steps * step));
            if (required == writtenBudget) return;
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
            stepCredit = 0;
        }

        internal static double TargetPhysicsRate(double fps)
        {
            const double shape = 6.411558187565301;
            if (fps >= 25) return 50;
            if (fps >= 12.5) return 25 + 25 * (1 - System.Math.Exp(-shape * (fps - 12.5) / 12.5)) / (1 - System.Math.Exp(-shape));
            if (fps >= 6.25)
            {
                double endSlope = shape / (12.5 * (1 - System.Math.Exp(-shape)));
                double power = (endSlope - 1 / 12.5) * 6.25 / System.Math.Log(2);
                return fps * System.Math.Exp(System.Math.Log(2) * System.Math.Pow((fps - 6.25) / 6.25, power));
            }
            return System.Math.Max(0, fps);
        }

        internal static double AllocateSteps(ref double credit, double desiredSteps)
        {
            credit += desiredSteps;
            double steps = System.Math.Max(1, System.Math.Floor(credit));
            credit = System.Math.Max(0, credit - steps);
            return steps;
        }

        internal static double FrameStepLimit(double fps, double desiredSteps, ref double credit)
        {
            if (fps <= 6.25)
            {
                credit = 0;
                return 1;
            }
            return fps >= 25 ? System.Math.Max(1, System.Math.Ceiling(desiredSteps))
                : AllocateSteps(ref credit, desiredSteps);
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
