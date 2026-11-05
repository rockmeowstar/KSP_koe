using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Networking;
using Debug=UnityEngine.Debug;

[assembly: AssemblyTitle("KerbalOverEngineering Database Scheduler")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace DatabaseSchedulerFix
{
    public static class Patch
    {
        const string Id="local.kerbaloverengineering.database-scheduler";
        static FieldInfo state;
        static readonly Dictionary<int,FieldInfo> loaders=new Dictionary<int,FieldInfo>();
        [ThreadStatic]static bool draining;
        static bool childWaits,readyAsync,frameBudget;
        public static long CompletedChildWaits,CompletedRequests,SynchronousChildren;
        sealed class CompletionListener
        {
            public void End(){Debug.Log("[DatabaseSchedulerFix] Complete: completedChildWaits="+CompletedChildWaits+"; completedRequests="+CompletedRequests+"; synchronousChildren="+SynchronousChildren);}
        }
        static readonly CompletionListener listener=new CompletionListener();
        public static readonly EventVoid.OnEvent CompletionCallback=listener.End;
        public static MethodInfo Bind()
        {
            if(typeof(GameDatabase).Assembly.ManifestModule.ModuleVersionId!=new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))throw new NotSupportedException("Unknown KSP database");
            var type=typeof(GameDatabase).GetNestedTypes(BindingFlags.Public|BindingFlags.NonPublic).Single(t=>t.Name.StartsWith("<LoadObjects>",StringComparison.Ordinal));
            state=AccessTools.Field(type,"<>1__state");
            if(state==null||state.FieldType!=typeof(int))throw new NotSupportedException("Database coroutine state changed");
            loaders.Clear();
            foreach(var pair in new[]{new[]{"3","<loader>5__12"},new[]{"5","<loader>5__14"}}){
                var field=AccessTools.Field(type,pair[1]);if(field==null)throw new NotSupportedException("Database loader field changed");loaders.Add(int.Parse(pair[0]),field);
            }
            var method=AccessTools.DeclaredMethod(type,"MoveNext");
            if(method==null||method.ReturnType!=typeof(bool))throw new NotSupportedException("Database MoveNext changed");
            return method;
        }
        public static void Apply()
        {
            childWaits=KerbalOverEngineering.Options.Settings.Enabled("Stock.DatabaseChildWaitDrain");
            readyAsync=KerbalOverEngineering.Options.Settings.Enabled("Stock.DatabaseReadyAsyncDrain");
            frameBudget=KerbalOverEngineering.Options.Settings.Enabled("Stock.DatabaseFrameBudget");
            if (!childWaits && !readyAsync && !frameBudget) return;
            var method=Bind();var info=Harmony.GetPatchInfo(method);if(info!=null&&info.Owners.Contains(Id))return;
            var h=new Harmony(Id);
            try{
                h.Patch(method,new HarmonyMethod(typeof(Patch),"Before"),new HarmonyMethod(typeof(Patch),"After"),new HarmonyMethod(typeof(Patch),"Transpile"));
                GameEvents.OnGameDatabaseLoaded.Add(CompletionCallback);
            }catch{h.UnpatchAll(Id);throw;}
        }
        static void Before(out long __state){__state=Stopwatch.GetTimestamp();}
        internal static float FrameBudget(float original){return KerbalOverEngineering.Options.LoadingResources.FrameBudget(original,Application.targetFrameRate,Screen.currentResolution.refreshRate);}
        static float GetFrameBudget(){return frameBudget ? FrameBudget(LoadingScreen.minFrameTime) : LoadingScreen.minFrameTime;}
        static bool CompletedChild(IEnumerator routine)
        {
            if(routine.Current!=null)return false;
            FieldInfo field;if(!loaders.TryGetValue((int)state.GetValue(routine),out field))return false;
            var loader=field.GetValue(routine);if(loader==null||loader.GetType().Assembly!=typeof(GameDatabase).Assembly)return false;
            var name=loader.GetType().FullName;
            return name=="DatabaseLoaderTexture_DDS"||name=="DatabaseLoaderTexture_PNG"||name=="DatabaseLoaderModel_MU";
        }
        static void After(IEnumerator __instance,ref bool __result,long __state)
        {
            if(draining || !childWaits)return;
            try{
                draining=true;double budget=Stopwatch.Frequency*GetFrameBudget();
                if(double.IsNaN(budget)||double.IsInfinity(budget)||budget<=0)return;
                while(__result&&CompletedChild(__instance)&&Stopwatch.GetTimestamp()-__state<budget){
                    CompletedChildWaits++;__result=__instance.MoveNext();
                }
            }finally{draining=false;}
        }
        internal static bool Known(Type type)
        {
            if((type.Assembly.GetName().Name=="PngPipelineFix"||type.Assembly.GetName().Name=="PngPipelineFix.Runtime")&&type.FullName.StartsWith("PngPipelineFix.Pipeline+<Run>",StringComparison.Ordinal))return true;
            if(type.Assembly!=typeof(GameDatabase).Assembly||type.DeclaringType==null)return false;
            var name=type.DeclaringType.FullName;
            return name=="DatabaseLoaderTexture_DDS"||name=="DatabaseLoaderTexture_PNG"||name=="DatabaseLoaderModel_MU";
        }
        static Coroutine StartChild(MonoBehaviour owner,IEnumerator routine)
        {
            if(!readyAsync || !Known(routine.GetType()))return owner.StartCoroutine(routine);
            try{
                while(routine.MoveNext()){
                    var operation=routine.Current as UnityWebRequestAsyncOperation;
                    if(operation!=null&&operation.isDone){CompletedRequests++;continue;}
                    return owner.StartCoroutine(Resume(routine,routine.Current));
                }
                SynchronousChildren++;
            }catch(Exception error){var failed=routine as IDisposable;if(failed!=null)failed.Dispose();Debug.LogException(error);return null;}
            var disposable=routine as IDisposable;if(disposable!=null)disposable.Dispose();return null;
        }
        static IEnumerator Resume(IEnumerator routine,object first)
        {
            try{yield return first;while(routine.MoveNext())yield return routine.Current;}
            finally{var disposable=routine as IDisposable;if(disposable!=null)disposable.Dispose();}
        }
        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code=source.Select(i=>new CodeInstruction(i)).ToList();int replaced=0,budgets=0;
            var target=AccessTools.Method(typeof(MonoBehaviour),"StartCoroutine",new[]{typeof(IEnumerator)});
            var frameTime=AccessTools.Field(typeof(LoadingScreen),"minFrameTime");
            foreach(var instruction in code){
                if(instruction.Calls(target)){
                    instruction.opcode=OpCodes.Call;instruction.operand=AccessTools.Method(typeof(Patch),"StartChild");replaced++;
                }else if(instruction.opcode==OpCodes.Ldsfld&&Equals(instruction.operand,frameTime)){
                    instruction.opcode=OpCodes.Call;instruction.operand=AccessTools.Method(typeof(Patch),"GetFrameBudget");budgets++;
                }
            }
            if(replaced!=3)throw new NotSupportedException("Database child coroutine calls changed: "+replaced);
            if(budgets!=4)throw new NotSupportedException("Database frame budget reads changed: "+budgets);
            return code;
        }
    }
    [KSPAddon(KSPAddon.Startup.Instantly,true)]
    public sealed class Bootstrap:MonoBehaviour
    {
        public void Awake()
        {
            try{Patch.Apply();Debug.Log("[DatabaseSchedulerFix] Loaded: stock DDS/PNG/MU, completed operations only; 8 ms/256-step drain; database-only 8 ms minimum frame budget; async waits retained.");}
            catch(Exception error){Debug.LogError("[DatabaseSchedulerFix] Disabled: "+error);}
            Destroy(this);
        }
    }
}
