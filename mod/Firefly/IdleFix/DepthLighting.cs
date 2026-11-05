using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using Firefly;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace FireflyIdleFix
{
    // The depth target never stores lighting. Restrict this path to the validated
    // part shader objects (including hash-pinned Deferred replacements).
    // Unknown shaders/vessels keep the original renderer.
    internal static class DepthLighting
    {
        sealed class Entry { public AtmoFxModule Module; public Vessel Vessel; public int Parts=-1; public Renderer[] Renderers; public GameObject[] Objects; }
        struct Frame { public Camera Camera; public bool Depth; }
        static readonly Dictionary<Camera,Entry> cameras=new Dictionary<Camera,Entry>();
        static readonly List<Frame> stack=new List<Frame>();
        static readonly List<Light> forcedLights=new List<Light>();
        static Light[] knownLights=new Light[0];
        static float nextLights;
        static readonly List<Material> materials=new List<Material>();
        static readonly HashSet<Material> checkedMaterials=new HashSet<Material>();
        static readonly HashSet<Shader> supported=new HashSet<Shader>();
        static readonly string[] shaderNames={"KSP/Bumped Specular","KSP/Emissive/Bumped Specular","KSP/Particles/Additive","KSP/Specular","KSP/Alpha/Translucent"};
        static PropertyInfo replacements;
        static bool replacementsReady;
        static readonly HashSet<string> warned=new HashSet<string>();
        static bool attached,failed,logged;
        static GameScenes scene;
        static int originalLights;
        static long applied;
        public static bool Bind(Harmony harmony)
        {
            if(SystemInfo.graphicsDeviceType!=GraphicsDeviceType.Direct3D11)return false;
            if(Environment.GetEnvironmentVariable("KOE_FIREFLY_DEPTH_DISABLE")=="1"||Environment.GetEnvironmentVariable("KOE_DEPTH_LIGHT_AUDIT")=="1")return false;
            var loader=AccessTools.TypeByName("Deferred.ShaderLoader");
            // All selected replacement Shader objects are byte-identical in
            // Deferred 1.3.3 and 1.3.5. Other changed shaders are not admitted.
            if(loader!=null&&KnownDeferred(loader.Assembly.Location))
                replacements=loader.GetProperty("ReplacementShaders",BindingFlags.Public|BindingFlags.Static);
            harmony.Patch(AccessTools.Method(typeof(AtmoFxModule),"CreateVesselFx"),postfix:new HarmonyMethod(typeof(DepthLighting),"Register"));
            Camera.onPreCull+=Before;Camera.onPostRender+=After;attached=true;scene=HighLogic.LoadedScene;return true;
        }
        internal static bool Matches(string path,string expected){if(!File.Exists(path))return false;using(var sha=SHA256.Create())using(var stream=File.OpenRead(path))return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","").ToLowerInvariant()==expected;}
        static bool KnownDeferred(string plugin)
        {
            string bundle=Path.Combine(Path.GetDirectoryName(plugin),"Shaders/replacementshaders.shab");
            return (Matches(plugin,"2f870bebde3b88ff0d051b816ad0b79c20b474acb96c28daa39e26e9edeed313")&&Matches(bundle,"a85e2a650112aa406d87b749f5f36cbcde932c6d65dab23e0cd062213d552bbd"))||
                (Matches(plugin,"47052420db8ddad759ba68eb53fb747c8f13335f1d253529b51d79cdf3b44108")&&Matches(bundle,"410f97b61be137953bf0be439e50203af88a470742b12c2591a97cb126cb6c71"));
        }
        static void Register(AtmoFxModule __instance)
        {
            if(!attached||!__instance.isLoaded||__instance.fxVessel==null||__instance.fxVessel.airstreamCamera==null)return;
            cameras[__instance.fxVessel.airstreamCamera]=new Entry{Module=__instance,Vessel=__instance.GetComponent<Vessel>()};
        }
        static bool Eligible(Camera camera)
        {
            Entry e;
            if(failed||!cameras.TryGetValue(camera,out e)||camera==null||camera.stereoEnabled||camera.actualRenderingPath!=RenderingPath.Forward||
                camera.targetTexture==null||camera.targetTexture.format!=RenderTextureFormat.Depth||camera.cullingMask!=1)return false;
            var module=e.Module;var vessel=e.Vessel;
            if(module==null||!module.isLoaded||module.OverridePhysics||vessel==null||vessel!=FlightGlobals.ActiveVessel||vessel.packed)return false;
            // Protect other loaded craft and unvalidated geometry/lighting interactions.
            foreach(var other in FlightGlobals.VesselsLoaded)if(other!=null&&other!=vessel&&other.loaded){if(warned.Add("multiple-loaded-vessels"))Debug.Log("[FireflyDepthLighting] Original path: another loaded vessel; "+other.vesselName+"; parts="+other.parts.Count);return false;}
            if(supported.Count==0)foreach(string name in shaderNames){var shader=Shader.Find(name);if(shader!=null)supported.Add(shader);}
            if(!replacementsReady&&replacements!=null){var shaders=replacements.GetValue(null,null) as Dictionary<string,Shader>;if(shaders!=null){foreach(string name in shaderNames){Shader shader;if(shaders.TryGetValue(name,out shader)&&shader!=null)supported.Add(shader);}replacementsReady=true;}}
            if(e.Parts!=vessel.parts.Count||e.Renderers==null){e.Parts=vessel.parts.Count;var all=new HashSet<Renderer>();foreach(var part in vessel.parts)foreach(var renderer in part.GetComponentsInChildren<Renderer>(true))all.Add(renderer);e.Renderers=new Renderer[all.Count];all.CopyTo(e.Renderers);e.Objects=new GameObject[e.Renderers.Length];for(int i=0;i<e.Renderers.Length;i++)e.Objects[i]=e.Renderers[i].gameObject;}
            if(e.Renderers.Length==0)return false;
            checkedMaterials.Clear();
            for(int i=0;i<e.Renderers.Length;i++)
            {
                var renderer=e.Renderers[i];if(renderer==null)continue;
                var obj=e.Objects[i];if(obj.layer!=0||!obj.activeInHierarchy||!renderer.enabled)continue;
                materials.Clear();renderer.GetSharedMaterials(materials);
                foreach(var material in materials)
                {
                    // No script executes between these reads: one shader lookup per
                    // shared material retains detection of changes on every render.
                    if(!checkedMaterials.Add(material))continue;
                    var shader=material==null?null:material.shader;
                    if(shader==null||(!supported.Contains(shader)&&!TufxDepthShader.Contains(shader)))
                    {
                        string name=shader==null?"missing":shader.name;
                        if(warned.Add(name))Debug.Log("[FireflyDepthLighting] Original path for shader: "+name+"; renderer="+renderer.name+"; actual="+(shader==null?0:shader.GetInstanceID())+"; lookup="+(Shader.Find(name)==null?0:Shader.Find(name).GetInstanceID()));
                        return false;
                    }
                }
            }
            return true;
        }
        static void Before(Camera camera)
        {
            try
            {
                bool depth=Eligible(camera);
                if(stack.Count==0&&!depth)return;
                if(stack.Count==0)
                {
                    originalLights=QualitySettings.pixelLightCount;
                    if(Time.realtimeSinceStartup>=nextLights){knownLights=UnityEngine.Object.FindObjectsOfType<Light>();nextLights=Time.realtimeSinceStartup+1f;}
                    forcedLights.Clear();
                    foreach(var light in knownLights)if(light!=null&&light.enabled&&(light.cullingMask&1)!=0&&light.renderMode==LightRenderMode.ForcePixel)forcedLights.Add(light);
                }
                // Nested reflection/other cameras always receive the original setting.
                stack.Add(new Frame{Camera=camera,Depth=depth});
                Lighting(depth);
                if(depth){applied++;if(!logged){logged=true;Debug.Log("[FireflyDepthLighting] Depth-only camera: pixel-light passes suppressed; original limit="+originalLights+"; forced lights="+forcedLights.Count);}}
            }
            catch(Exception ex){failed=true;Restore();Debug.LogError("[FireflyDepthLighting] Original path restored: "+ex);}
        }
        static void After(Camera camera)
        {
            if(stack.Count==0)return;
            int index=stack.Count-1;while(index>=0&&stack[index].Camera!=camera)index--;
            if(index<0)return;
            stack.RemoveRange(index,stack.Count-index);Lighting(stack.Count>0&&stack[stack.Count-1].Depth);
            if(stack.Count==0)forcedLights.Clear();
        }
        static void Lighting(bool depth){QualitySettings.pixelLightCount=depth?0:originalLights;foreach(var light in forcedLights)if(light!=null)light.renderMode=depth?LightRenderMode.ForceVertex:LightRenderMode.ForcePixel;}
        static void Restore(){if(stack.Count>0){Lighting(false);stack.Clear();forcedLights.Clear();}}
        public static IEnumerator EndFrames(){while(attached){yield return new WaitForEndOfFrame();Restore();}}
        public static void FrameStart(){Restore();if(scene!=HighLogic.LoadedScene){scene=HighLogic.LoadedScene;cameras.Clear();checkedMaterials.Clear();materials.Clear();knownLights=new Light[0];nextLights=0;}}
        public static void Stop()
        {
            Restore();if(!attached)return;attached=false;Camera.onPreCull-=Before;Camera.onPostRender-=After;cameras.Clear();checkedMaterials.Clear();materials.Clear();knownLights=new Light[0];
            Debug.Log("[FireflyDepthLighting] Applied camera renders="+applied);
        }
    }
}
