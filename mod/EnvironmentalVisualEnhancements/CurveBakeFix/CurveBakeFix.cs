using System;
using System.Linq;
using System.Reflection;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering EVE Curve Bake Fix")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace EveCurveBakeFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        bool attached;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("EVE.CurveBakeHoist"))
            {
                Destroy(this);
                return;
            }

            string mode = Environment.GetEnvironmentVariable("KOE_EVE_CURVES_MODE");
            if (!string.IsNullOrEmpty(mode) && mode != "0")
            {
                Destroy(this);
                return;
            }

            try
            {
                var assembly = AppDomain.CurrentDomain.GetAssemblies().Single(a => a.GetName().Name == "Atmosphere");
                EveCurveKernel.Apply(assembly, 3, Log);
                DontDestroyOnLoad(gameObject);
                GameEvents.onGameSceneLoadRequested.Add(SceneRequested);
                attached = true;
                Debug.Log("[EveCurveBakeFix] Loaded 1.0.0: invariant hardness/density interpolation once per column; stock accumulation order retained");
            }
            catch (Exception ex)
            {
                Debug.LogError("[EveCurveBakeFix] Disabled: " + ex.Message);
                Destroy(this);
            }
        }

        static void Log(string kind, string message)
        {
            if (kind == "EVE_CURVE_COUNTERS")
                Debug.Log("[EveCurveBakeFix] " + message);
        }

        public void SceneRequested(GameScenes scene)
        {
            EveCurveKernel.Report();
        }

        public void OnApplicationQuit()
        {
            EveCurveKernel.Report();
        }

        public void OnDestroy()
        {
            if (attached)
                GameEvents.onGameSceneLoadRequested.Remove(SceneRequested);
        }
    }
}
