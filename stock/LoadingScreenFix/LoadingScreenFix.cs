using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Loading Screen Fix")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace LoadingScreenFix
{
    [KSPAddon(KSPAddon.Startup.Instantly,true)]
    public sealed class Bootstrap:MonoBehaviour
    {
        static MethodInfo show,hide;
        static int generation;
        static GameScenes guiScene;
        static PropertyInfo scattererInstance;
        static FieldInfo core,reader,bodies,bodyName,bodyActive,proland,skyNode,skyInit,atmoLoaded,shadersInit,scaled;
        static bool active;
        public static long Reveals, Fallbacks, Cancelled;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.ReadySceneReveal")) { Destroy(this); return; }
            try
            {
                if(typeof(LoadingBufferMask).Assembly.ManifestModule.ModuleVersionId!=new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))throw new NotSupportedException("Unknown KSP");
                var assembly=AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a=>string.Equals(a.GetName().Name,"scatterer",StringComparison.OrdinalIgnoreCase));
                if(assembly!=null)
                {
                    if(assembly.ManifestModule.ModuleVersionId!=new Guid("0c7cdd85-3385-4e7f-a3ad-efb7387ea6e1"))throw new NotSupportedException("Unknown Scatterer readiness API");
                    var type=assembly.GetType("Scatterer.Scatterer",true);
                    scattererInstance=AccessTools.Property(type,"Instance");core=Field(type,"coreInitiated");reader=Field(type,"planetsConfigsReader");
                    bodies=Field(reader.FieldType,"scattererCelestialBodies");
                    var body=assembly.GetType("Scatterer.ScattererCelestialBody",true);
                    bodyName=Field(body,"celestialBodyName");bodyActive=Field(body,"active");proland=Field(body,"prolandManager");skyNode=Field(proland.FieldType,"skyNode");
                    skyInit=Field(skyNode.FieldType,"skyNodeInitiated");atmoLoaded=Field(skyNode.FieldType,"precomputedAtmoLoaded");shadersInit=Field(skyNode.FieldType,"atmosphereShadersInitiated");scaled=Field(skyNode.FieldType,"inScaledSpace");
                    if(scattererInstance==null)throw new NotSupportedException("Scatterer singleton changed");
                }
                show=AccessTools.DeclaredMethod(typeof(LoadingBufferMask),"Show");hide=AccessTools.DeclaredMethod(typeof(LoadingBufferMask),"Hide");
                new Harmony("local.kerbaloverengineering.loading-screen").Patch(AccessTools.DeclaredMethod(typeof(LoadingBufferMask),"ShowDuration"),null,new HarmonyMethod(typeof(Bootstrap),"Replace"));
                GameEvents.onLevelWasLoadedGUIReady.Add(Gui);GameEvents.onGameSceneLoadRequested.Add(Request);
                active=true;DontDestroyOnLoad(gameObject);
                Debug.Log("[LoadingScreenFix] Ready-driven KSC and prelaunch reveal; atmosphere, camera and physics guards retained.");
            }
            catch(Exception ex){new Harmony("local.kerbaloverengineering.loading-screen").UnpatchAll("local.kerbaloverengineering.loading-screen");Debug.LogError("[LoadingScreenFix] Disabled: "+ex.Message);Destroy(this);}
        }
        static FieldInfo Field(Type type,string name)
        {var field=AccessTools.Field(type,name);if(field==null)throw new NotSupportedException(type.Name+"."+name+" changed");return field;}
        void Gui(GameScenes scene){guiScene=scene;}
        void Request(GameScenes scene){generation++;guiScene=GameScenes.LOADING;}
        static bool EffectsReady(GameScenes scene,string home)
        {
            if(scattererInstance==null)return true;
            var instance=scattererInstance.GetValue(null,null);
            if(instance==null||!(bool)core.GetValue(instance))return false;
            foreach(var body in (IEnumerable)bodies.GetValue(reader.GetValue(instance)))
            {
                if((string)bodyName.GetValue(body)!=home)continue;
                if(!(bool)bodyActive.GetValue(body))return false;
                var manager=proland.GetValue(body);if(manager==null)return false;
                var sky=skyNode.GetValue(manager);if(sky==null)return false;
                return (bool)skyInit.GetValue(sky)&&(bool)atmoLoaded.GetValue(sky)&&(bool)shadersInit.GetValue(sky)&&
                    (scene!=GameScenes.SPACECENTER||!(bool)scaled.GetValue(sky));
            }
            return true;
        }
        static void Replace(LoadingBufferMask __instance,ref IEnumerator __result)
        {
            var scene=HighLogic.LoadedScene;
            if(scene==GameScenes.SPACECENTER||scene==GameScenes.FLIGHT)__result=Wait(__instance,scene,generation,__result);
        }
        static IEnumerator Wait(LoadingBufferMask mask,GameScenes scene,int token,IEnumerator stock)
        {
            show.Invoke(mask,null);float start=Time.realtimeSinceStartup;int stable=0;
            while(token==generation&&HighLogic.LoadedScene==scene)
            {
                bool ready=false,fallback=false;
                try
                {
                    ready=guiScene==scene&&Camera.main!=null&&Camera.main.isActiveAndEnabled;
                    string home=Planetarium.fetch.Home.name;
                    if(scene==GameScenes.FLIGHT)
                    {
                        var vessel=FlightGlobals.ActiveVessel;
                        ready=ready&&FlightGlobals.ready&&vessel!=null&&vessel.loaded&&!vessel.packed&&FlightCamera.fetch!=null&&InputLockManager.GetControlLock("PhysicsEase")==0;
                        if(vessel!=null){fallback=vessel.situation!=Vessel.Situations.PRELAUNCH;home=vessel.mainBody.name;}
                    }
                    ready=ready&&EffectsReady(scene,home);
                }
                catch(Exception ex){fallback=true;Debug.LogWarning("[LoadingScreenFix] Stock wait: "+ex.Message);}
                if(fallback||Time.realtimeSinceStartup-start>30)
                {
                    Fallbacks++;
                    while(token==generation&&HighLogic.LoadedScene==scene&&stock.MoveNext())yield return stock.Current;
                    yield break;
                }
                stable=ready?stable+1:0;
                if(stable>=2)
                {
                    yield return new WaitForEndOfFrame();
                    if(token==generation&&HighLogic.LoadedScene==scene){Reveals++;hide.Invoke(mask,null);}else Cancelled++;
                    yield break;
                }
                yield return null;
            }
            Cancelled++;
        }
        public void OnDestroy()
        {if(active){GameEvents.onLevelWasLoadedGUIReady.Remove(Gui);GameEvents.onGameSceneLoadRequested.Remove(Request);active=false;}}
    }
}
