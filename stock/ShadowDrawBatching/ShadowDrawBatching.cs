using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using KerbalOverEngineering.Options;

namespace KerbalOverEngineering.ShadowDrawBatching
{
    public sealed class Bootstrap : MonoBehaviour
    {
        [DllImport("kernel32",CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
        [DllImport("kernel32",CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module,string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl,CharSet=CharSet.Unicode)] delegate int Initialize(string shader);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Mode(uint enabled);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Clear();
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate IntPtr RenderEvent();
        Mode mode;
        Clear clear;
        IntPtr renderEvent;
        bool camerasSubscribed,scenesSubscribed;
        static T Function<T>(IntPtr module,string name) where T:class
        {
            IntPtr address=GetProcAddress(module,name);
            if(address==IntPtr.Zero)throw new MissingMethodException(name);
            return Marshal.GetDelegateForFunctionPointer(address,typeof(T)) as T;
        }
        public void Awake()
        {
            if(!Settings.Enabled("Rendering.ShadowDrawBatching")||GetModuleHandleW("renderdoc.dll")!=IntPtr.Zero){Destroy(gameObject);return;}
            try{
                string folder=NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module=LoadLibraryW(Path.Combine(folder,"KoeShadowBatches.dll"));
                if(module==IntPtr.Zero)throw new InvalidOperationException("Shadow native adapter did not load");
                int result=Function<Initialize>(module,"InitializeShadowBatches")(Path.Combine(folder,"banked-vs.dxbc"));
                if(result!=0)throw new InvalidOperationException("Shadow initialization: "+result);
                mode=Function<Mode>(module,"ShadowMode");clear=Function<Clear>(module,"ClearShadowBatchCaches");
                renderEvent=Function<RenderEvent>(module,"GetRenderEventFunc")();
                if(renderEvent==IntPtr.Zero||mode(1)!=0)throw new InvalidOperationException("Shadow adapter could not start");
                Camera.onPostRender+=PostRender;camerasSubscribed=true;
                GameEvents.onGameSceneLoadRequested.Add(SceneLoad);scenesSubscribed=true;
                Debug.Log("[KOE ShadowBatches] enabled; GPU matrix bank, Rust geometry packets, original-draw recovery");
            }catch(Exception error){if(mode!=null)mode(0);Debug.LogError("[KOE ShadowBatches] disabled: "+error);Destroy(gameObject);}
        }
        void PostRender(Camera camera){if(renderEvent!=IntPtr.Zero)GL.IssuePluginEvent(renderEvent,1);}
        void SceneLoad(GameScenes scene){if(clear!=null)clear();}
        public void OnDestroy(){if(camerasSubscribed)Camera.onPostRender-=PostRender;if(scenesSubscribed)GameEvents.onGameSceneLoadRequested.Remove(SceneLoad);if(mode!=null)mode(0);}
        public void OnApplicationQuit(){if(mode!=null)mode(0);}
    }
}
