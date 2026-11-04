using System;
using System.Reflection;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering PQS Noise Pipeline")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace PqsNoiseFix
{
    [KSPAddon(KSPAddon.Startup.Instantly,true)]
    public sealed class Bootstrap:MonoBehaviour
    {
        bool applied;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.PqsNoisePreparation")) { Destroy(this); return; }
            string mode=Environment.GetEnvironmentVariable("KOE_PQS_NOISE_MODE");
            if(!string.IsNullOrEmpty(mode)&&mode!="0"){Destroy(this);return;}
            try{
                NoiseKernel.Apply(3,Log,KerbalOverEngineering.Options.Settings.Enabled("Stock.PqsPatchStateMemo"));applied=true;DontDestroyOnLoad(gameObject);
                Debug.Log("[PqsNoiseFix] Loaded: supported Simplex generators on any body; worker-owned clones; original callbacks and arithmetic retained");
            }catch(Exception ex){Debug.LogError("[PqsNoiseFix] Disabled: "+ex.Message);Destroy(this);}
        }
        static void Log(string kind,string message)
        {
            if(kind=="PQS_NOISE_FALLBACK")Debug.LogWarning("[PqsNoiseFix] "+kind+" "+message);
            else Debug.Log("[PqsNoiseFix] "+kind+" "+message);
        }
        public void OnApplicationQuit(){if(applied)NoiseKernel.Report();}
    }
}
