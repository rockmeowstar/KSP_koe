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
    public sealed class Bridge : MonoBehaviour
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        delegate int Initialize(string library);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Set(int on, int scene);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Toggle(int on);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Action();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Read([Out] long[] stats, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void ReadUI([Out] long[] stats);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Integer();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate IntPtr Pointer();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Texture(IntPtr image);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Scalar(float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate double MonitorRefresh(IntPtr window);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void TargetMode(int fullGeneration, double targetFPS);
        Texture SetFrameGenerationSilhouette;
        Texture SetFrameGenerationCloudLayer;
        Texture SetFrameGenerationWaterLayer;
        Scalar SetFrameGenerationCloudFade;
        UnityEngine.Texture cloudTexture;
        IntPtr cloudNative;
        IntPtr sentSilhouette, sentCloud, sentWater;
        readonly Dictionary<UnityEngine.Texture, IntPtr> cloudHandles = new Dictionary<UnityEngine.Texture, IntPtr>();
        int cloudWidth, cloudHeight;
        readonly VesselSilhouette silhouette = new VesselSilhouette();
        readonly EffectLayers effectLayers = new EffectLayers();
        readonly IndependentEffectPass independentEffects = new IndependentEffectPass();
        readonly IndependentSkyPass independentSky=new IndependentSkyPass();
        readonly IndependentWaterfallPass independentWaterfall=new IndependentWaterfallPass();
        readonly bool independentLayers = Environment.GetEnvironmentVariable("KOE_INDEPENDENT_LAYERS")=="1";
        readonly EveCloudLayer eveCloudLayer = new EveCloudLayer();
        readonly AtmosphereSourceProbe atmosphereProbe=new AtmosphereSourceProbe();
        AtmosphereSourceProbe.Send SendAtmosphereProbe;
        readonly OceanLayer oceanLayer = new OceanLayer();
        readonly Dictionary<Camera, CommandBuffer> layerBegins = new Dictionary<Camera, CommandBuffer>();
        readonly Dictionary<Camera, CommandBuffer> layerEnds = new Dictionary<Camera, CommandBuffer>();
        readonly Dictionary<Camera, CommandBuffer> stencilBegins = new Dictionary<Camera, CommandBuffer>();
        readonly Dictionary<Camera, CommandBuffer> stencilCaptures = new Dictionary<Camera, CommandBuffer>();
        Read ReadFrameGenerationStencil;
        bool stencilPrototype;
        readonly long[] stencilStats = new long[3];
        Read ReadFrameGenerationIndependent;
        Integer CanFrameGenerationIndependentCapture;
        readonly long[] independentStats = new long[11];
        readonly long[] previousIndependentStats = new long[11];
        readonly Dictionary<Camera, CommandBuffer> silhouetteBuffers = new Dictionary<Camera, CommandBuffer>();
        Initialize InitializeFrameGeneration;
        Set SetFrameGeneration;
        Action InvalidateFrameGenerationHistory, StopFrameGeneration;
        Read ReadFrameGeneration;
        ReadUI ReadFrameGenerationUI;
        Integer GetFrameGenerationCaptureFormat;
        Read ReadFrameGenerationPresentation;
        Read ReadFrameGenerationReShade;
        Read ReadFrameGenerationQuality;
        Read ReadFrameGenerationSilhouette;
        Read ReadFrameGenerationTarget;
        Read ReadFrameGenerationLayers;
        readonly long[] layerStats = new long[6];
        int layerFrame = -1, layerDraws, lastLayerDraws;
        double layerCpuMs, lastLayerCpuMs;
        readonly long[] targetStats = new long[5];
        readonly long[] silhouetteStats = new long[2];
        readonly long[] quality = new long[17];
        readonly long[] reshade = new long[3];
        readonly long[] presentation = new long[3], lastPresentation = new long[3];
        Pointer GetFrameGenerationError, GetFrameGenerationRenderEvent;
        static T Function<T>(IntPtr module, string name)
            where T : class
        {
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero)
                throw new MissingMethodException(name);
            return Marshal.GetDelegateForFunctionPointer(address, typeof(T)) as T;
        }

        sealed class UiBuffer
        {
            public Camera Camera;
            public CommandBuffer Buffer;
        }

        readonly Dictionary<int, UiBuffer> uiBuffers = new Dictionary<int, UiBuffer>();
        IntPtr renderEvent;
        readonly long[] values = new long[10], previous = new long[10];
        readonly long[] ui = new long[2];
        bool initialized, on = true;
        string trace;
        float next, last;
        int frames, fixedTicks;
        Camera historyCamera;
        Vessel historyVessel;
        CelestialBody historyBody;
        Vector3 historyOffset;
        Quaternion historyRotation;
        float historyFov;
        void CheckFlightHistory()
        {
            Camera camera = FlightCamera.fetch == null ? null : FlightCamera.fetch.mainCamera;
            Vessel vessel = FlightGlobals.ActiveVessel;
            if (!HighLogic.LoadedSceneIsFlight || camera == null || vessel == null)
            {
                historyCamera = null;
                historyVessel = null;
                return;
            }

            // AI-generated comment: Relative position survives KSP floating-origin shifts. Absolute
            // coordinates would invalidate history during a normal launch.
            Vector3 offset = camera.transform.position - vessel.transform.position;
            bool cut = historyCamera != null
                && (camera != historyCamera
                || vessel != historyVessel
                || vessel.mainBody != historyBody
                || Quaternion.Angle(historyRotation, camera.transform.rotation) > 25
                || Mathf.Abs(camera.fieldOfView - historyFov) > 10
                || (offset - historyOffset).magnitude > Mathf.Max(10, historyOffset.magnitude * 0.35f));
            if (cut)
            {
                InvalidateFrameGenerationHistory();
                Debug.Log("[KoeFrameGeneration] camera/vessel cut: temporal history discarded");
            }

            historyCamera = camera;
            historyVessel = vessel;
            historyBody = vessel.mainBody;
            historyOffset = offset;
            historyRotation = camera.transform.rotation;
            historyFov = camera.fieldOfView;
        }

        static string F(double value)
        {
            return value.ToString("F6", CultureInfo.InvariantCulture);
        }

        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Rendering.FrameGeneration"))
            {
                Destroy(gameObject);
                return;
            }

            try
            {
                if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11)
                    throw new NotSupportedException("DX11 required");
                string folder = KerbalOverEngineering.Options.NativeAssets.Extract(typeof(Bridge).Assembly);
                IntPtr module = LoadLibraryW(Path.Combine(folder, "KoeFrameGeneration.dll"));
                if (module == IntPtr.Zero)
                    throw new InvalidOperationException("native loader " + Marshal.GetLastWin32Error());
                InitializeFrameGeneration = Function<Initialize>(module, "InitializeFrameGeneration");
                SetFrameGeneration = Function<Set>(module, "SetFrameGeneration");
                InvalidateFrameGenerationHistory = Function<Action>(module, "InvalidateFrameGenerationHistory");
                StopFrameGeneration = Function<Action>(module, "StopFrameGeneration");
                ReadFrameGeneration = Function<Read>(module, "ReadFrameGeneration");
                ReadFrameGenerationUI = Function<ReadUI>(module, "ReadFrameGenerationUI");
                GetFrameGenerationCaptureFormat = Function<Integer>(module, "GetFrameGenerationCaptureFormat");
                GetFrameGenerationError = Function<Pointer>(module, "GetFrameGenerationError");
                GetFrameGenerationRenderEvent = Function<Pointer>(module, "GetFrameGenerationRenderEvent");
                if (GetProcAddress(module, "ReadFrameGenerationPresentation") != IntPtr.Zero)
                    ReadFrameGenerationPresentation = Function<Read>(module, "ReadFrameGenerationPresentation");
                if (GetProcAddress(module, "ReadFrameGenerationReShade") != IntPtr.Zero)
                    ReadFrameGenerationReShade = Function<Read>(module, "ReadFrameGenerationReShade");
                ReadFrameGenerationQuality = Function<Read>(module, "ReadFrameGenerationQuality");
                SetFrameGenerationSilhouette = Function<Texture>(module, "SetFrameGenerationSilhouette");
                SetFrameGenerationCloudLayer = Function<Texture>(module, "SetFrameGenerationCloudLayer");
                if(independentLayers)
                {
                    SetFrameGenerationCloudFade=Function<Scalar>(module,"SetFrameGenerationCloudFade");
                    ReadFrameGenerationIndependent=Function<Read>(module,"ReadFrameGenerationIndependent");
                    CanFrameGenerationIndependentCapture=Function<Integer>(module,"CanFrameGenerationIndependentCapture");
                    SendAtmosphereProbe=Function<AtmosphereSourceProbe.Send>(module,"SetFrameGenerationAtmosphereProbe");
                }
                SetFrameGenerationWaterLayer = Function<Texture>(module, "SetFrameGenerationWaterLayer");
                ReadFrameGenerationSilhouette = Function<Read>(module, "ReadFrameGenerationSilhouette");
                ReadFrameGenerationTarget = Function<Read>(module, "ReadFrameGenerationTarget");
                ReadFrameGenerationLayers = Function<Read>(module, "ReadFrameGenerationLayers");
                if(DeferredStencilProvider.RequestedAndSupported())
                {
                    ReadFrameGenerationStencil=Function<Read>(module,"ReadFrameGenerationStencil");
                    stencilPrototype=true;
                }
                double monitorHz;
                using (var process = System.Diagnostics.Process.GetCurrentProcess())
                    monitorHz = Function<MonitorRefresh>(module, "GetFrameGenerationMonitorHz")(process.MainWindowHandle);
                var target = FrameGenerationSettings.Load(Path.Combine(KSPUtil.ApplicationRootPath,
                    "GameData", "KerbalOverEngineering", "FrameGeneration.cfg"), monitorHz);
                Function<Toggle>(module, "SetAdaptiveFrameCadence")(KerbalOverEngineering.Options.Settings.Enabled("Rendering.AdaptiveFrameCadence") ? 1 : 0);
                Function<TargetMode>(module, "ConfigureFrameGenerationTarget")(target.FullGeneration ? 1 : 0, target.TargetFPS);
                // AI-generated comment: The direct-flow backend does not read this path, but the native
                // shared initializer still normalizes it to an absolute path.
                int status = InitializeFrameGeneration(folder);
                if (status != 0)
                    throw new InvalidOperationException("native init " + status
                        + ": " + Marshal.PtrToStringAnsi(GetFrameGenerationError()));
                if (ReadFrameGenerationReShade != null)
                {
                    ReadFrameGenerationReShade(reshade, reshade.Length);
                    Debug.Log("[KoeFrameGeneration] ReShade API integration=" + (reshade[0] != 0));
                }

                initialized = true;
                trace = Environment.GetEnvironmentVariable("KOE_FRAME_GENERATION_TRACE");
                last = Time.realtimeSinceStartup;
                next = last + 2;
                DontDestroyOnLoad(gameObject);
                renderEvent = GetFrameGenerationRenderEvent();
                Camera.onPreCull += ConfigureUI;
                oceanLayer.RenderEvent = renderEvent;
                oceanLayer.SourceReady = SendWaterLayer;
                Camera.onPreRender += ObserveLayerSources;
                GameEvents.onVesselWasModified.Add(silhouette.Invalidate);
                GameEvents.onGameSceneLoadRequested.Add(ClearUI);
                Debug.Log(
                    "[KoeFrameGeneration] enabled DX11 GPU interpolation; F10 toggles; base rendering/physics unchanged; presentation counters are submissions, not monitor scanout"
                );
            }
            catch (Exception e)
            {
                Debug.LogError("[KoeFrameGeneration] disabled: " + e);
                Destroy(this);
            }
        }

        void ConfigureUI(Camera camera)
        {
            BeginLayerFrame();
            long begin = System.Diagnostics.Stopwatch.GetTimestamp();
            try { ConfigureCamera(camera); }
            finally { layerCpuMs += 1000.0 * (System.Diagnostics.Stopwatch.GetTimestamp()-begin) / System.Diagnostics.Stopwatch.Frequency; }
        }

        void BeginLayerFrame()
        {
            if (layerFrame == Time.frameCount) return;
            lastLayerCpuMs=layerCpuMs;lastLayerDraws=layerDraws;
            layerCpuMs=0;layerDraws=0;layerFrame=Time.frameCount;
            if (sentCloud != IntPtr.Zero) { SetFrameGenerationCloudLayer(IntPtr.Zero); sentCloud = IntPtr.Zero; }
            if (sentWater != IntPtr.Zero) { SetFrameGenerationWaterLayer(IntPtr.Zero); sentWater = IntPtr.Zero; }
        }

        void ObserveLayerSources(Camera camera)
        {
            if (!on || (!HighLogic.LoadedSceneIsFlight && HighLogic.LoadedScene != GameScenes.SPACECENTER)) return;
            BeginLayerFrame();
            long begin = System.Diagnostics.Stopwatch.GetTimestamp();
            try { eveCloudLayer.Observe(camera); oceanLayer.Observe(camera); SendCloudLayer();
                if(independentLayers && CanFrameGenerationIndependentCapture()!=0)
                {independentSky.Prepare(camera,renderEvent);independentWaterfall.Prepare(camera,renderEvent);}
                if(SendAtmosphereProbe!=null)atmosphereProbe.Observe(camera,SendAtmosphereProbe); }
            finally { layerCpuMs += 1000.0 * (System.Diagnostics.Stopwatch.GetTimestamp()-begin) / System.Diagnostics.Stopwatch.Frequency; }
        }

        void ConfigureCamera(Camera camera)
        {
            // AI-generated comment: Inspect the final camera pose after LateUpdate, before its image
            // is captured. Update alone observes a camera cut one frame late.
            if (HighLogic.LoadedSceneIsFlight && FlightCamera.fetch != null && camera == FlightCamera.fetch.mainCamera)
            {
                CheckFlightHistory();
            }
            if (camera != null && camera.targetTexture == null && !camera.orthographic
                && camera.cullingMask != ((1 << 5) | (1 << 25)))
                ConfigureSilhouette(camera);
            // AI-generated comment: This stock camera draws only UI layers. Capture the completed world
            // before any of its geometry; final swap-chain pixels supply the UI.
            ConfigureCleanWorld(camera);
        }

        void SendWaterLayer(IntPtr water)
        {
            if (!on) water = IntPtr.Zero;
            if (water != sentWater) { SetFrameGenerationWaterLayer(water); sentWater = water; }
        }

        void SendCloudLayer()
        {
            UnityEngine.Texture clouds = eveCloudLayer.Image;
            if (cloudWidth != Screen.width || cloudHeight != Screen.height)
            {
                cloudWidth = Screen.width; cloudHeight = Screen.height;
                cloudHandles.Clear(); cloudTexture = null;
            }
            if (clouds != cloudTexture)
            {
                cloudTexture = clouds;
                cloudNative = IntPtr.Zero;
                if (clouds != null && clouds.width > 2 && clouds.height > 2
                    && !cloudHandles.TryGetValue(clouds, out cloudNative))
                {
                    cloudNative = clouds.GetNativeTexturePtr(); cloudHandles.Add(clouds, cloudNative);
                }
            }
            IntPtr cloud = on ? cloudNative : IntPtr.Zero;
            if (cloud != sentCloud) { SetFrameGenerationCloudLayer(cloud); sentCloud = cloud; }
            if(SetFrameGenerationCloudFade!=null)SetFrameGenerationCloudFade(eveCloudLayer.Fade);
        }

        void ConfigureCleanWorld(Camera camera)
        {
            if (camera == null
                || camera.name != "UIMainCamera"
                || camera.targetTexture != null
                || camera.cullingMask != ((1 << 5) | (1 << 25))
                || uiBuffers.ContainsKey(camera.GetInstanceID()))
                return;
            var commands = new CommandBuffer
            {
                name = "KOE clean world before UI"
            };
            commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            commands.IssuePluginEvent(renderEvent, 1);
            camera.AddCommandBuffer(CameraEvent.BeforeForwardOpaque, commands);
            uiBuffers.Add(camera.GetInstanceID(), new UiBuffer { Camera = camera, Buffer = commands });
            Debug.Log("[KoeFrameGeneration] UI excluded: pre-UI world input; original UI pixels composited after interpolation");
        }

        void ClearUI(GameScenes scene)
        {
            ClearSilhouette();
            if (initialized)
                SetFrameGeneration(on ? 1 : 0, 0);
            foreach (var item in uiBuffers.Values)
            {
                if (item.Camera != null)
                    item.Camera.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque, item.Buffer);
                item.Buffer.Dispose();
            }

            uiBuffers.Clear();
        }

        void ConfigureSilhouette(Camera camera)
        {
            if (!on || !silhouette.Prepare(camera)) return;
            if(independentLayers)
            {
                if(CanFrameGenerationIndependentCapture()!=0)independentEffects.Prepare(camera,renderEvent);
                else independentEffects.Dispose();
            }
            else effectLayers.Prepare(camera, renderEvent);
            if (silhouette.NativeImage != sentSilhouette)
            { SetFrameGenerationSilhouette(silhouette.NativeImage); sentSilhouette = silhouette.NativeImage; }
            if (!layerBegins.ContainsKey(camera))
            {
                var beginCommands = new CommandBuffer { name = "KOE layer frame identity" };
                layerBegins.Add(camera, beginCommands);
                camera.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, beginCommands);
                var endCommands = new CommandBuffer { name = "KOE translucent coverage end" };
                endCommands.IssuePluginEvent(renderEvent, 10);
                layerEnds.Add(camera, endCommands);
                camera.AddCommandBuffer(CameraEvent.AfterForwardAlpha, endCommands);
            }
            CommandBuffer frameStart = layerBegins[camera];
            Camera flightCamera=FlightCamera.fetch==null?null:FlightCamera.fetch.mainCamera;
            bool stencilScene=stencilPrototype && (HighLogic.LoadedScene==GameScenes.SPACECENTER
                || (HighLogic.LoadedSceneIsFlight && flightCamera!=null
                && flightCamera.actualRenderingPath==RenderingPath.DeferredShading));
            bool alphaSource=!stencilScene || camera==flightCamera
                || camera.actualRenderingPath==RenderingPath.DeferredShading
                || (camera.cullingMask&silhouette.CelestialLayers)!=0;
            frameStart.Clear();
            frameStart.IssuePluginEvent(renderEvent, 6 | ((Time.frameCount & 0x07ffffff) << 3));
            bool localStencil=stencilScene && camera.actualRenderingPath==RenderingPath.DeferredShading
                && (independentLayers || !HighLogic.LoadedSceneIsFlight || camera==flightCamera);
            layerEnds[camera].Clear();
            if(alphaSource)
            {
                frameStart.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                frameStart.IssuePluginEvent(renderEvent,independentLayers&&!localStencil?20:9);
                layerEnds[camera].IssuePluginEvent(renderEvent,10);
            }
            if(localStencil)
            {
                ReadFrameGenerationStencil(stencilStats,stencilStats.Length);
                if(stencilStats[2]>0)
                {
                    stencilPrototype=false;ClearStencilCameras();InvalidateFrameGenerationHistory();localStencil=false;
                    Debug.LogWarning("[KoeFrameGeneration] Native stencil source unavailable; renderer masks restored");
                }
            }
            if(localStencil)
            {
                CommandBuffer before;
                if(!stencilBegins.TryGetValue(camera,out before))
                {
                    before=new CommandBuffer{name="KOE stencil frame identity"};stencilBegins.Add(camera,before);
                    camera.AddCommandBuffer(CameraEvent.BeforeGBuffer,before);
                    var capture=new CommandBuffer{name="KOE existing Deferred stencil"};
                    stencilCaptures.Add(camera,capture);camera.AddCommandBuffer(CameraEvent.AfterGBuffer,capture);
                }
                before.Clear();before.IssuePluginEvent(renderEvent,6|((Time.frameCount&0x07ffffff)<<3));
                if(independentLayers)before.IssuePluginEvent(renderEvent,19);
                bool flip=Environment.GetEnvironmentVariable("KOE_LAYER_STENCIL_FLIP")!="0";
                bool clear=camera.clearFlags==CameraClearFlags.Color||camera.clearFlags==CameraClearFlags.Skybox;
                int options=(clear?1:0)|(flip?2:0)|(SystemInfo.usesReversedZBuffer?4:0);
                CommandBuffer captureCommands=stencilCaptures[camera];captureCommands.Clear();
                captureCommands.IssuePluginEvent(renderEvent,11|(options<<8));
                return;
            }
            CommandBuffer commands;
            if (!silhouetteBuffers.TryGetValue(camera, out commands))
            {
                commands = new CommandBuffer { name = "KOE scene layers" };
                silhouetteBuffers.Add(camera, commands);
                camera.AddCommandBuffer(CameraEvent.AfterEverything, commands);
            }
            commands.Clear();
            commands.IssuePluginEvent(renderEvent, 5);
            layerDraws += silhouette.Record(commands, camera,stencilScene && stencilPrototype);
            commands.IssuePluginEvent(renderEvent, 2);
        }

        void ClearSilhouette()
        {
            ClearStencilCameras();
            effectLayers.Dispose();
            independentEffects.Dispose();
            independentSky.Dispose();
            independentWaterfall.Dispose();
            eveCloudLayer.Clear();
            atmosphereProbe.Clear();
            if(SendAtmosphereProbe!=null)SendAtmosphereProbe(IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,-1,new float[4]);
            oceanLayer.Clear();
            cloudTexture = null; cloudNative = IntPtr.Zero;
            cloudHandles.Clear();
            if (SetFrameGenerationCloudLayer != null) SetFrameGenerationCloudLayer(IntPtr.Zero);
            if (SetFrameGenerationWaterLayer != null) SetFrameGenerationWaterLayer(IntPtr.Zero);
            sentSilhouette = sentCloud = sentWater = IntPtr.Zero;
            if (SetFrameGenerationSilhouette != null) SetFrameGenerationSilhouette(IntPtr.Zero);
            foreach (var entry in silhouetteBuffers)
            {
                if (entry.Key != null)
                {
                    entry.Key.RemoveCommandBuffer(CameraEvent.AfterEverything, entry.Value);
                }
                entry.Value.Dispose();
            }
            silhouetteBuffers.Clear();
            foreach (var entry in layerBegins)
            {
                if (entry.Key != null) entry.Key.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, entry.Value);
                entry.Value.Dispose();
            }
            layerBegins.Clear();
            foreach (var entry in layerEnds)
            {
                if (entry.Key != null) entry.Key.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha, entry.Value);
                entry.Value.Dispose();
            }
            layerEnds.Clear();
            silhouette.Dispose();
        }

        void ClearStencilCameras()
        {
            foreach(var entry in stencilBegins)
            {if(entry.Key!=null)entry.Key.RemoveCommandBuffer(CameraEvent.BeforeGBuffer,entry.Value);entry.Value.Dispose();}
            foreach(var entry in stencilCaptures)
            {if(entry.Key!=null)entry.Key.RemoveCommandBuffer(CameraEvent.AfterGBuffer,entry.Value);entry.Value.Dispose();}
            stencilBegins.Clear();stencilCaptures.Clear();
        }

        public void Update()
        {
            if (!initialized)
                return;
            if (Input.GetKeyDown(KeyCode.F10))
                on = !on;
            bool scene = HighLogic.LoadedSceneIsFlight || HighLogic.LoadedScene == GameScenes.SPACECENTER;
            if (!HighLogic.LoadedSceneIsFlight)
            {
                historyCamera = null;
                historyVessel = null;
            }

            SetFrameGeneration(on ? 1 : 0, scene ? 1 : 0);
            if ((!on || !scene) && (silhouetteBuffers.Count>0 || layerBegins.Count>0 || stencilCaptures.Count>0)) ClearSilhouette();
            frames++;
            if (Time.realtimeSinceStartup < next)
                return;
            ReadFrameGeneration(values, values.Length);
            ReadFrameGenerationUI(ui);
            float now = Time.realtimeSinceStartup;
            double seconds = now - last;
            string row = "{\"t\":" + F(now)
                + ",\"scene\":\"" + HighLogic.LoadedScene
                + "\",\"enabled\":" + values[0]
                + ",\"allowed\":" + values[1]
                + ",\"baseFPS\":" + F(frames / seconds)
                + ",\"capturedFPS\":" + F((values[2] - previous[2]) / seconds)
                + ",\"generated\":" + values[3]
                + ",\"realPresents\":" + values[4]
                + ",\"generatedPresents\":" + values[5]
                + ",\"submittedPresentFPS\":" + F((values[4] - previous[4] + values[5] - previous[5]) / seconds)
                + ",\"skipped\":" + values[6]
                + ",\"repeated\":" + values[7]
                + ",\"errors\":" + values[8]
                + ",\"workerMs\":" + F(values[9] / 1000.0)
                + "}\n";
            row = row.TrimEnd('\r', '\n', '}')
                + ",\"fixedUpdateHz\":" + F(fixedTicks / seconds)
                + ",\"cleanCopies\":" + ui[0]
                + ",\"missingClean\":" + ui[1]
                + ",\"captureFormats\":" + GetFrameGenerationCaptureFormat()
                + "}\n";
            if (ReadFrameGenerationPresentation != null)
            {
                ReadFrameGenerationPresentation(presentation, presentation.Length);
                long calls = presentation[0] - lastPresentation[0];
                row = row.TrimEnd('\r', '\n', '}')
                    + ",\"mainPresentWaitMs\":" + F(calls > 0 ? (presentation[1] - lastPresentation[1]) / (1000.0 * calls) : 0)
                    + ",\"zeroSyncPresents\":" + (presentation[2] - lastPresentation[2])
                    + ",\"qualityVsync\":" + QualitySettings.vSyncCount
                    + "}\n";
                Array.Copy(presentation, lastPresentation, presentation.Length);
            }

            if (ReadFrameGenerationReShade != null)
            {
                ReadFrameGenerationReShade(reshade, reshade.Length);
                row = row.TrimEnd('\r', '\n', '}')
                    + ",\"reshadeActive\":" + reshade[0]
                    + ",\"reshadeCaptures\":" + reshade[1]
                    + ",\"reshadePrivatePresents\":" + reshade[2]
                    + "}\n";
            }

            ReadFrameGenerationQuality(quality, quality.Length);
            ReadFrameGenerationSilhouette(silhouetteStats, silhouetteStats.Length);
            ReadFrameGenerationTarget(targetStats, targetStats.Length);
            ReadFrameGenerationLayers(layerStats, layerStats.Length);
            if(ReadFrameGenerationStencil!=null)ReadFrameGenerationStencil(stencilStats,stencilStats.Length);
            row = row.TrimEnd('\r', '\n', '}')
                + ",\"layerCpuMs\":" + F(lastLayerCpuMs)
                + ",\"layerDraws\":" + lastLayerDraws
                + ",\"layerGeometryGpuMs\":" + F(layerStats[0] / 1000.0)
                + ",\"layerEffectCopyGpuMs\":" + F(layerStats[1] / 1000.0)
                + ",\"layerMergeGpuMs\":" + F(layerStats[2] / 1000.0)
                + ",\"layerContourGpuMs\":" + F(layerStats[3] / 1000.0)
                + ",\"fireflyLayerPairs\":" + layerStats[4]
                + ",\"optionalLayerFailures\":" + layerStats[5]
                + ",\"nativeStencilMode\":" + (stencilPrototype?"true":"false")
                + ",\"stencilSuccesses\":" + stencilStats[1]
                + ",\"stencilFailures\":" + stencilStats[2]
                + ",\"cloudLayerActive\":" + (cloudNative != IntPtr.Zero ? "true" : "false")
                + ",\"waterLayerActive\":" + (sentWater != IntPtr.Zero ? "true" : "false")
                + ",\"requestedTargetFPS\":" + F(targetStats[0] / 1000.0)
                + ",\"fullGeneration\":" + (targetStats[4] != 0 ? "true" : "false")
                + ",\"monitorHz\":" + F(targetStats[1] / 1000.0)
                + ",\"effectiveTargetFPS\":" + F(targetStats[2] / 1000.0)
                + ",\"scheduledPhases\":" + targetStats[3]
                + ",\"silhouetteCopies\":" + silhouetteStats[0]
                + ",\"silhouetteFrames\":" + silhouetteStats[1]
                + ",\"qualityRequests\":" + quality[0]
                + ",\"qualityAccepted\":" + quality[1]
                + ",\"qualityNoHistory\":" + quality[2]
                + ",\"qualityLate\":" + quality[4]
                + ",\"qualityStatic\":" + quality[5]
                + ",\"qualityMotionRejected\":" + quality[6]
                + ",\"qualityContourRejected\":" + quality[7]
                + ",\"qualityInvalid\":" + quality[8]
                + ",\"flowExecutions\":" + quality[9]
                + ",\"preflightVetoes\":" + quality[10]
                + ",\"inputWaitMs\":" + F(quality[11] / 1000.0)
                + ",\"decisionAgeMs\":" + F(quality[12] / 1000.0)
                + ",\"analyzedPixels\":" + quality[13]
                + ",\"safePixels\":" + quality[14]
                + ",\"synthesizedPixels\":" + quality[15]
                + ",\"composedPixels\":" + quality[16]
                + "}\n";
            if(ReadFrameGenerationIndependent!=null)
            {
                ReadFrameGenerationIndependent(independentStats,independentStats.Length);
                row=row.TrimEnd('\r','\n','}')
                    +",\"independentLayers\":true"
                    +",\"independentPacked\":"+independentStats[1]
                    +",\"independentUnavailable\":"+independentStats[2]
                    +",\"independentFireflyCopies\":"+independentStats[3]
                    +",\"independentParityRejected\":"+independentStats[4]
                    +",\"presentedChangedPixels\":"+independentStats[5]
                    +",\"presentedComposedPixels\":"+independentStats[6]
                    +",\"presentedPixelTotal\":"+independentStats[7]
                    +",\"dxgiDisplayedFPS\":"+F((independentStats[8]-previousIndependentStats[8])/seconds)
                    +",\"dxgiDisplayedFrames\":"+independentStats[8]
                    +",\"dxgiDisplayStatsFailures\":"+independentStats[9]
                    +",\"dxgiDisplayStatsSamples\":"+independentStats[10]+"}\n";
                Array.Copy(independentStats,previousIndependentStats,independentStats.Length);
            }
            if (!string.IsNullOrEmpty(trace))
                File.AppendAllText(trace, row);
            if (values[8] > previous[8])
                Debug.LogError("[KoeFrameGeneration] " + Marshal.PtrToStringAnsi(GetFrameGenerationError()));
            Array.Copy(values, previous, values.Length);
            frames = 0;
            fixedTicks = 0;
            last = now;
            next = now + 2;
        }

        public void FixedUpdate()
        {
            if (initialized)
                fixedTicks++;
        }

        public void OnApplicationQuit()
        {
            if (initialized)
            {
                Camera.onPreCull -= ConfigureUI;
                Camera.onPreRender -= ObserveLayerSources;
                oceanLayer.Dispose();
                GameEvents.onGameSceneLoadRequested.Remove(ClearUI);
                GameEvents.onVesselWasModified.Remove(silhouette.Invalidate);
                ClearUI(HighLogic.LoadedScene);
                StopFrameGeneration();
                initialized = false;
            }
        }

        public void OnDestroy()
        {
            OnApplicationQuit();
        }
    }
}
