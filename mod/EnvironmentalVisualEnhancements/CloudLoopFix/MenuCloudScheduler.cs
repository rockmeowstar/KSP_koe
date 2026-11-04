using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace CloudLoopFix
{
    // Unity work stays on the main thread. A config is applied by the original EVE method.
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class MenuCloudScheduler : MonoBehaviour
    {
        sealed class Job
        {
            public object Handler, Objects;
            public ConfigNode[] Nodes;
            public int Cursor;
            public bool Complete;
        }
        static readonly Dictionary<object,Job> jobs=new Dictionary<object,Job>();
        static MethodInfo apply;
        static FieldInfo nodes, raymarched, loaded;
        static bool active;
        public static long QueuedBodies, AppliedNodes, CompletedBodies, TransitionDrains, CancelledBodies;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("EVE.MenuInitScheduler")) { Destroy(this); return; }
            const string id="local.kerbaloverengineering.eve-menu-scheduler";
            var harmony=new Harmony(id);
            try
            {
                var assembly=AppDomain.CurrentDomain.GetAssemblies().Single(a=>a.GetName().Name=="Atmosphere");
                if(assembly.ManifestModule.ModuleVersionId!=new Guid("8150e218-893c-4b2c-a852-b674a3006cfd"))throw new NotSupportedException("Unknown EVE");
                var type=assembly.GetType("Atmosphere.CelestialBodyCloudsHandler",true);
                apply=AccessTools.DeclaredMethod(type,"ApplyConfigNode");
                nodes=AccessTools.Field(type,"configNodes");raymarched=AccessTools.Field(type,"hasRaymarchedVolumetrics");loaded=AccessTools.Field(type,"isLoaded");
                if(apply==null||nodes==null||raymarched==null||loaded==null)throw new NotSupportedException("EVE loading API changed");
                harmony.Patch(AccessTools.DeclaredMethod(type,"LoadBody"),new HarmonyMethod(typeof(MenuCloudScheduler),"Queue"));
                harmony.Patch(AccessTools.DeclaredMethod(type,"Update"),new HarmonyMethod(typeof(MenuCloudScheduler),"Complete"),new HarmonyMethod(typeof(MenuCloudScheduler),"Suppress"));
                harmony.Patch(AccessTools.DeclaredMethod(assembly.GetType("Atmosphere.CloudsManager",true),"Clean"),new HarmonyMethod(typeof(MenuCloudScheduler),"Cancel"));
                GameEvents.onGameSceneLoadRequested.Add(Drain);
                active=true;DontDestroyOnLoad(gameObject);
                Debug.Log("[CloudLoopFix] Menu scheduler: one original cloud config per frame; completion and transitions guarded.");
            }
            catch(Exception ex){harmony.UnpatchAll(id);Debug.LogError("[CloudLoopFix] Menu scheduler disabled: "+ex);Destroy(this);}
        }
        static bool Queue(object __instance,object objectList)
        {
            if(!active||HighLogic.LoadedScene!=GameScenes.MAINMENU)return true;
            if(jobs.ContainsKey(__instance))return false;
            bool useRaymarch=(bool)raymarched.GetValue(__instance);
            var selected=((IEnumerable<ConfigNode>)nodes.GetValue(__instance)).Where(n=>n.HasNode("layerRaymarchedVolumeV5")||!useRaymarch).ToArray();
            jobs.Add(__instance,new Job{Handler=__instance,Objects=objectList,Nodes=selected});QueuedBodies++;
            return false;
        }
        static void Step(Job job)
        {
            if(job.Complete)return;
            if(job.Cursor<job.Nodes.Length)
            {
                var node=job.Nodes[job.Cursor++];
                try{apply.Invoke(job.Handler,new[]{(object)node,job.Objects});}
                catch(TargetInvocationException ex){Debug.LogError("Unable to parse config node:\n"+node+"\nException:\n"+ex.InnerException);}
                AppliedNodes++;
            }
            if(job.Cursor==job.Nodes.Length){loaded.SetValue(job.Handler,true);job.Complete=true;CompletedBodies++;}
        }
        public void Update()
        {
            if(HighLogic.LoadedScene!=GameScenes.MAINMENU)return;
            var job=jobs.Values.FirstOrDefault(j=>!j.Complete);if(job!=null)Step(job);
        }
        static bool Complete(object __instance,ref bool __result)
        {
            Job job;if(!jobs.TryGetValue(__instance,out job)||!job.Complete)return true;
            jobs.Remove(__instance);__result=true;return false;
        }
        static void Suppress(object __instance,ref bool __result)
        {Job job;if(jobs.TryGetValue(__instance,out job)&&!job.Complete)__result=false;}
        void Drain(GameScenes scene)
        {
            if(scene==GameScenes.MAINMENU)return;
            foreach(var job in jobs.Values.ToArray())if(!job.Complete){TransitionDrains++;while(!job.Complete)Step(job);}
        }
        static void Cancel(){CancelledBodies+=jobs.Count;jobs.Clear();}
        public void OnDestroy()
        {if(active){GameEvents.onGameSceneLoadRequested.Remove(Drain);Cancel();active=false;}}
    }
}
