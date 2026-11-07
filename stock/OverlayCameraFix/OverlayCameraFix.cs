using System;
using HarmonyLib;
using UnityEngine;

namespace KoeOverlayLifetime
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.Stock.OverlayCameraCleanup";
        public static long Released;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.OverlayCameraCleanup")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                if (typeof(FlightVesselOverlays).Module.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                    throw new NotSupportedException("Stock overlay lifetime has not been reviewed for this assembly");
                var destroy = AccessTools.DeclaredMethod(typeof(FlightVesselOverlays), "OnDestroy");
                harmony.Patch(destroy, new HarmonyMethod(typeof(Bootstrap), "Before"), null, null, new HarmonyMethod(typeof(Bootstrap), "After"));
                Debug.Log("[KOE OverlayLifetime] Owned overlay cameras are destroyed with their controller");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE OverlayLifetime] Disabled: " + error.Message); }
            Destroy(this);
        }
        static void Before(FlightVesselOverlays __instance, out Camera __state) { __state = __instance.overlayCamera; }
        static Exception After(Camera __state, Exception __exception)
        {
            if (__state != null) { Destroy(__state.gameObject); Released++; }
            return __exception;
        }
    }
}
