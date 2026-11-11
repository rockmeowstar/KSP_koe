using UnityEngine;
using System.Reflection;

namespace KerbalOverEngineering.Options
{
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public sealed class PhysicsWarpPrecision : MonoBehaviour
    {
        const string Option = "Stock.PhysicsWarpPrecision";
        public static bool PatchesReady;
        public static bool IsAccelerated { get { return IsActive && Time.timeScale > 1; } }
        static PhysicsWarpPrecision instance;
        public static bool IsActive
        {
            get { return instance != null && instance.active && instance.ownsStep && PatchesReady
                && Time.fixedDeltaTime == instance.baseStep && instance.planetarium != null
                && instance.planetarium.fixedDeltaTime == instance.writtenClockStep
                && Time.timeScale > 0 && (TimeWarp.WarpMode == TimeWarp.Modes.LOW || TimeWarp.CurrentRate == 1); }
        }
        bool active;
        bool ownsStep;
        float baseStep;
        Planetarium planetarium;
        double originalClockStep;
        double writtenClockStep;
        bool pendingWarp, pendingInstant, pendingMessage;
        int pendingRate;
        TimeWarp.Modes pendingMode;
        static readonly MethodInfo setMode = typeof(TimeWarp).GetMethod("setMode", BindingFlags.Instance | BindingFlags.NonPublic,
            null, new[] { typeof(TimeWarp.Modes) }, null);

        public static bool DeferWarp(int rate, bool instant, bool message)
        {
            if (!IsActive || !Time.inFixedTimeStep || setMode == null) return false;
            instance.pendingRate = rate;
            instance.pendingInstant = instant;
            instance.pendingMessage = message;
            instance.pendingWarp = true;
            instance.pendingMode = TimeWarp.WarpMode;
            return true;
        }

        public void Awake()
        {
            instance = this;
            active = PatchesReady && Settings.Enabled(Option) && Settings.Enabled("Stock.RealtimePhysicsCatchup");
            GameEvents.onTimeWarpRateChanged.Add(RateChanged);
            RateChanged();
        }

        void RateChanged()
        {
            if (!active || !PatchesReady || Time.timeScale <= 0
                || (TimeWarp.WarpMode != TimeWarp.Modes.LOW && TimeWarp.CurrentRate != 1))
            {
                Restore();
                return;
            }
            float step = Time.fixedDeltaTime;
            if (!ownsStep)
            {
                baseStep = step / Time.timeScale;
                planetarium = Planetarium.fetch;
                if (planetarium == null) return;
                originalClockStep = planetarium.fixedDeltaTime;
            }
            if (baseStep <= 0 || float.IsNaN(baseStep) || float.IsInfinity(baseStep)) return;
            if (!(Planetarium.TimeScale > 0) || double.IsInfinity(Planetarium.TimeScale)
                || !(originalClockStep > 0) || double.IsInfinity(originalClockStep)) return;
            Time.fixedDeltaTime = baseStep;
            writtenClockStep = originalClockStep / Planetarium.TimeScale;
            planetarium.fixedDeltaTime = writtenClockStep;
            ownsStep = true;
        }

        void Restore()
        {
            if (ownsStep && Time.fixedDeltaTime == baseStep)
                Time.fixedDeltaTime = baseStep * (Time.timeScale > 0 ? Time.timeScale : 1);
            if (ownsStep && planetarium != null && planetarium.fixedDeltaTime == writtenClockStep)
                planetarium.fixedDeltaTime = originalClockStep;
            ownsStep = false;
        }

        public void OnDisable() { pendingWarp = false; Restore(); }
        public void OnEnable() { if (active) RateChanged(); }

        void LateUpdate()
        {
            if (!ownsStep) return;
            if (PatchesReady && Time.fixedDeltaTime == baseStep && planetarium != null
                && planetarium.fixedDeltaTime == writtenClockStep)
            {
                if (pendingWarp)
                {
                    pendingWarp = false;
                    if (TimeWarp.WarpMode != pendingMode
                        && !(bool)setMode.Invoke(TimeWarp.fetch, new object[] { pendingMode }))
                    {
                        Debug.Log("[KOE PhysicsWarpPrecision] Deferred warp cancelled: requested mode unavailable");
                        return;
                    }
                    TimeWarp.SetRate(pendingRate, pendingInstant, pendingMessage);
                }
                return;
            }
            Restore();
            active = false;
            pendingWarp = false;
            Debug.Log("[KOE PhysicsWarpPrecision] Suspended after another component changed the physics step or clock");
        }

        public void OnDestroy()
        {
            Restore();
            GameEvents.onTimeWarpRateChanged.Remove(RateChanged);
            if (ReferenceEquals(instance, this)) instance = null;
        }
    }
}
