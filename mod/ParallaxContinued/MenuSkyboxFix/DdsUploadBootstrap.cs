using System;
using UnityEngine;

namespace MenuSkyboxFix
{
    [KSPAddon(KSPAddon.Startup.Instantly,true)]
    public sealed class DdsUploadBootstrap:MonoBehaviour
    {
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("ParallaxContinued.DdsDirectUpload")) { Destroy(this); return; }
            try{DdsPayloadPatch.Apply();Debug.Log("[MenuSkyboxFix] DDS payload: original parser, one file buffer, direct pinned upload; no payload array/copy.");}
            catch(Exception ex){Debug.LogError("[MenuSkyboxFix] DDS payload disabled: "+ex.Message);}
            Destroy(this);
        }
    }
}
