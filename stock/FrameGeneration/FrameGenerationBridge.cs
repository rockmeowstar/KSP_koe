using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    public sealed class Bridge:MonoBehaviour
    {
        [DllImport("kernel32",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32",CharSet=CharSet.Ansi,ExactSpelling=true)]static extern IntPtr GetProcAddress(IntPtr module,string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl,CharSet=CharSet.Unicode)]delegate int Initialize(string library);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate void Set(int on,int scene);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate void Toggle(int on);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate void Action();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate void Read([Out]long[] stats,int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate void ReadUI([Out]long[] stats);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate int Integer();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate IntPtr Pointer();
        Initialize InitializeFrameGeneration;Set SetFrameGeneration;Action InvalidateFrameGenerationHistory,StopFrameGeneration;
        Read ReadFrameGeneration;ReadUI ReadFrameGenerationUI;Integer GetFrameGenerationCaptureFormat;
        Read ReadFrameGenerationPresentation;
        Read ReadFrameGenerationReShade;
        readonly long[] reshade=new long[3];
        readonly long[] presentation=new long[3],lastPresentation=new long[3];
        Pointer GetFrameGenerationError,GetFrameGenerationRenderEvent;
        static T Function<T>(IntPtr module,string name)where T:class
        {
            IntPtr address=GetProcAddress(module,name);if(address==IntPtr.Zero)throw new MissingMethodException(name);
            return Marshal.GetDelegateForFunctionPointer(address,typeof(T)) as T;
        }
        sealed class UiBuffer{public Camera Camera;public CommandBuffer Buffer;}
        readonly Dictionary<int,UiBuffer> uiBuffers=new Dictionary<int,UiBuffer>();IntPtr renderEvent;
        readonly long[] values=new long[10],previous=new long[10];
        readonly long[] ui=new long[2];
        bool initialized,on=true;string trace;float next,last;int frames,fixedTicks;
        Camera historyCamera;Vessel historyVessel;Vector3 historyOffset;Quaternion historyRotation;float historyFov;
        void CheckFlightHistory()
        {
            Camera camera=FlightCamera.fetch==null?null:FlightCamera.fetch.mainCamera;
            Vessel vessel=FlightGlobals.ActiveVessel;
            if(!HighLogic.LoadedSceneIsFlight||camera==null||vessel==null){historyCamera=null;historyVessel=null;return;}
            // Relative position survives KSP floating-origin shifts. Absolute
            // coordinates would invalidate history during a normal launch.
            Vector3 offset=camera.transform.position-vessel.transform.position;
            bool cut=historyCamera!=null&&(camera!=historyCamera||vessel!=historyVessel||
                Quaternion.Angle(historyRotation,camera.transform.rotation)>25||
                Mathf.Abs(camera.fieldOfView-historyFov)>10||
                (offset-historyOffset).magnitude>Mathf.Max(10,historyOffset.magnitude*0.35f));
            if(cut){InvalidateFrameGenerationHistory();Debug.Log("[KoeFrameGeneration] camera/vessel cut: temporal history discarded");}
            historyCamera=camera;historyVessel=vessel;historyOffset=offset;historyRotation=camera.transform.rotation;historyFov=camera.fieldOfView;
        }
        static string F(double value){return value.ToString("F6",CultureInfo.InvariantCulture);}
        public void Awake()
        {
            if(!KerbalOverEngineering.Options.Settings.Enabled("Rendering.FrameGeneration")){Destroy(gameObject);return;}
            try{
                if(SystemInfo.graphicsDeviceType!=GraphicsDeviceType.Direct3D11)throw new NotSupportedException("DX11 required");
                string folder=KerbalOverEngineering.Options.NativeAssets.Extract(typeof(Bridge).Assembly);
                IntPtr module=LoadLibraryW(Path.Combine(folder,"KoeFrameGeneration.dll"));
                if(module==IntPtr.Zero)throw new InvalidOperationException("native loader "+Marshal.GetLastWin32Error());
                InitializeFrameGeneration=Function<Initialize>(module,"InitializeFrameGeneration");
                SetFrameGeneration=Function<Set>(module,"SetFrameGeneration");
                InvalidateFrameGenerationHistory=Function<Action>(module,"InvalidateFrameGenerationHistory");
                StopFrameGeneration=Function<Action>(module,"StopFrameGeneration");
                ReadFrameGeneration=Function<Read>(module,"ReadFrameGeneration");
                ReadFrameGenerationUI=Function<ReadUI>(module,"ReadFrameGenerationUI");
                GetFrameGenerationCaptureFormat=Function<Integer>(module,"GetFrameGenerationCaptureFormat");
                GetFrameGenerationError=Function<Pointer>(module,"GetFrameGenerationError");
                GetFrameGenerationRenderEvent=Function<Pointer>(module,"GetFrameGenerationRenderEvent");
                if(GetProcAddress(module,"ReadFrameGenerationPresentation")!=IntPtr.Zero)ReadFrameGenerationPresentation=Function<Read>(module,"ReadFrameGenerationPresentation");
                if(GetProcAddress(module,"ReadFrameGenerationReShade")!=IntPtr.Zero)ReadFrameGenerationReShade=Function<Read>(module,"ReadFrameGenerationReShade");
                Function<Toggle>(module,"SetAdaptiveFrameCadence")(KerbalOverEngineering.Options.Settings.Enabled("Rendering.AdaptiveFrameCadence")?1:0);
                // The direct-flow backend does not read this path, but the native
                // shared initializer still normalizes it to an absolute path.
                int status=InitializeFrameGeneration(folder);
                if(status!=0)throw new InvalidOperationException("native init "+status+": "+Marshal.PtrToStringAnsi(GetFrameGenerationError()));
                if(ReadFrameGenerationReShade!=null){ReadFrameGenerationReShade(reshade,reshade.Length);Debug.Log("[KoeFrameGeneration] ReShade API integration="+(reshade[0]!=0));}
                initialized=true;trace=Environment.GetEnvironmentVariable("KOE_FRAME_GENERATION_TRACE");last=Time.realtimeSinceStartup;next=last+2;DontDestroyOnLoad(gameObject);
                renderEvent=GetFrameGenerationRenderEvent();Camera.onPreCull+=ConfigureUI;GameEvents.onGameSceneLoadRequested.Add(ClearUI);
                Debug.Log("[KoeFrameGeneration] enabled DX11 GPU interpolation; F10 toggles; base rendering/physics unchanged; presentation counters are submissions, not monitor scanout");
            }catch(Exception e){Debug.LogError("[KoeFrameGeneration] disabled: "+e);Destroy(this);}
        }
        void ConfigureUI(Camera camera)
        {
            // Inspect the final camera pose after LateUpdate, before its image
            // is captured. Update alone observes a camera cut one frame late.
            if(HighLogic.LoadedSceneIsFlight&&FlightCamera.fetch!=null&&camera==FlightCamera.fetch.mainCamera)CheckFlightHistory();
            // This stock camera draws only UI layers. Capture the completed world
            // before any of its geometry; final swap-chain pixels supply the UI.
            if(camera==null||camera.name!="UIMainCamera"||camera.targetTexture!=null||camera.cullingMask!=((1<<5)|(1<<25))||uiBuffers.ContainsKey(camera.GetInstanceID()))return;
            var commands=new CommandBuffer{name="KOE clean world before UI"};commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);commands.IssuePluginEvent(renderEvent,1);
            camera.AddCommandBuffer(CameraEvent.BeforeForwardOpaque,commands);uiBuffers.Add(camera.GetInstanceID(),new UiBuffer{Camera=camera,Buffer=commands});
            Debug.Log("[KoeFrameGeneration] UI excluded: pre-UI world input; original UI pixels composited after interpolation");
        }
        void ClearUI(GameScenes scene){if(initialized)SetFrameGeneration(on?1:0,0);foreach(var item in uiBuffers.Values){if(item.Camera!=null)item.Camera.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque,item.Buffer);item.Buffer.Dispose();}uiBuffers.Clear();}
        public void Update()
        {
            if(!initialized)return;
            if(Input.GetKeyDown(KeyCode.F10))on=!on;
            bool scene=HighLogic.LoadedSceneIsFlight||HighLogic.LoadedScene==GameScenes.SPACECENTER;
            if(!HighLogic.LoadedSceneIsFlight){historyCamera=null;historyVessel=null;}
            SetFrameGeneration(on?1:0,scene?1:0);frames++;
            if(Time.realtimeSinceStartup<next)return;
            ReadFrameGeneration(values,values.Length);ReadFrameGenerationUI(ui);float now=Time.realtimeSinceStartup;double seconds=now-last;
            string row="{\"t\":"+F(now)+",\"scene\":\""+HighLogic.LoadedScene+"\",\"enabled\":"+values[0]+",\"allowed\":"+values[1]+",\"baseFPS\":"+F(frames/seconds)+",\"capturedFPS\":"+F((values[2]-previous[2])/seconds)+",\"generated\":"+values[3]+",\"realPresents\":"+values[4]+",\"generatedPresents\":"+values[5]+",\"submittedPresentFPS\":"+F((values[4]-previous[4]+values[5]-previous[5])/seconds)+",\"skipped\":"+values[6]+",\"repeated\":"+values[7]+",\"errors\":"+values[8]+",\"workerMs\":"+F(values[9]/1000.0)+"}\n";
            row=row.TrimEnd('\r','\n','}')+",\"fixedUpdateHz\":"+F(fixedTicks/seconds)+",\"cleanCopies\":"+ui[0]+",\"missingClean\":"+ui[1]+",\"captureFormats\":"+GetFrameGenerationCaptureFormat()+"}\n";
            if(ReadFrameGenerationPresentation!=null){
                ReadFrameGenerationPresentation(presentation,presentation.Length);
                long calls=presentation[0]-lastPresentation[0];
                row=row.TrimEnd('\r','\n','}')+",\"mainPresentWaitMs\":"+F(calls>0?(presentation[1]-lastPresentation[1])/(1000.0*calls):0)+",\"zeroSyncPresents\":"+(presentation[2]-lastPresentation[2])+",\"qualityVsync\":"+QualitySettings.vSyncCount+"}\n";
                Array.Copy(presentation,lastPresentation,presentation.Length);
            }
            if(ReadFrameGenerationReShade!=null){ReadFrameGenerationReShade(reshade,reshade.Length);row=row.TrimEnd('\r','\n','}')+",\"reshadeActive\":"+reshade[0]+",\"reshadeCaptures\":"+reshade[1]+",\"reshadePrivatePresents\":"+reshade[2]+"}\n";}
            if(!string.IsNullOrEmpty(trace))File.AppendAllText(trace,row);
            if(values[8]>previous[8])Debug.LogError("[KoeFrameGeneration] "+Marshal.PtrToStringAnsi(GetFrameGenerationError()));
            Array.Copy(values,previous,values.Length);frames=0;fixedTicks=0;last=now;next=now+2;
        }
        public void FixedUpdate(){if(initialized)fixedTicks++;}
        public void OnApplicationQuit(){if(initialized){Camera.onPreCull-=ConfigureUI;GameEvents.onGameSceneLoadRequested.Remove(ClearUI);ClearUI(HighLogic.LoadedScene);StopFrameGeneration();initialized=false;}}
        public void OnDestroy(){OnApplicationQuit();}
    }
}
