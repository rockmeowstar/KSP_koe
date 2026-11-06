using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class OceanLayer
    {
        sealed class Capture
        {
            public Camera Camera;
            public Component Source;
            public CommandBuffer Commands;
            public RenderTexture Image;
            public IntPtr Native;
        }
        readonly Dictionary<Camera, Capture> captures = new Dictionary<Camera, Capture>();
        Type type;
        FieldInfo active;
        bool resolved;
        bool hookAttempted;
        object harmony;
        static OceanLayer hookOwner;
        int frame = -1;
        IntPtr image;
        public IntPtr RenderEvent;
        public Action<IntPtr> SourceReady;
        readonly string traceRoot=Environment.GetEnvironmentVariable("KOE_LAYER_DATASET_ROOT");
        readonly HashSet<string> traceEvents=new HashSet<string>();
        public IntPtr NativeImage { get { return frame == Time.frameCount ? image : IntPtr.Zero; } }

        public void Observe(Camera camera)
        {
            if(camera==null)return;
            if (camera.orthographic || camera.cameraType != CameraType.Game
                || camera.pixelWidth != Screen.width || camera.pixelHeight != Screen.height)
            {Trace("cameraRejected",camera);return;}
            if (frame != Time.frameCount) { frame = Time.frameCount; image = IntPtr.Zero; }
            if (!resolved)
            {
                resolved = true;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if ((type = assembly.GetType("Scatterer.OceanCommandBuffer", false)
                        ?? assembly.GetType("scatterer.OceanCommandBuffer", false)) != null) break;
                if (type != null) active = type.GetField("renderingEnabled", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            }
            if (type == null || active == null) return;
            BindHook();
            Capture capture;
            if (!captures.TryGetValue(camera, out capture))
            {
                Component source = camera.GetComponent(type);
                if (source == null) {Trace("sourceAbsent",camera);return;}
                capture = new Capture { Camera = camera, Source = source,
                    Commands = new CommandBuffer { name = "KOE ocean coverage copy" } };
                camera.AddCommandBuffer(CameraEvent.BeforeForwardAlpha, capture.Commands);
                captures.Add(camera, capture);
            }
            capture.Commands.Clear();
            if(capture.Source==null)capture.Source=camera.GetComponent(type);
            if (capture.Source == null || !(bool)active.GetValue(capture.Source)) {Trace("sourceInactive",camera);return;}
            Trace("sourceActive",camera);
            if (capture.Image == null || capture.Image.width != camera.pixelWidth || capture.Image.height != camera.pixelHeight)
            {
                if (capture.Image != null) { capture.Image.Release(); UnityEngine.Object.Destroy(capture.Image); }
                capture.Image = new RenderTexture(camera.pixelWidth, camera.pixelHeight, 0,
                    RenderTextureFormat.RFloat, RenderTextureReadWrite.Linear)
                    { name = "KOE ocean coverage", hideFlags = HideFlags.HideAndDontSave, filterMode = FilterMode.Point };
                if (!capture.Image.Create()) return;
                capture.Native = capture.Image.GetNativeTexturePtr();
            }
            // AI-generated comment: Scatterer writes this depth using the real projected wave
            // grid and scene depth. Copy it before camera-end releases temporary targets.
            if (RenderEvent != IntPtr.Zero) capture.Commands.IssuePluginEvent(RenderEvent, 7);
            capture.Commands.CopyTexture(new RenderTargetIdentifier(Shader.PropertyToID("oceanGbufferDepth")),
                new RenderTargetIdentifier(capture.Image));
            if (RenderEvent != IntPtr.Zero) capture.Commands.IssuePluginEvent(RenderEvent, 8);
            image = capture.Native;
            Trace("sourceReady",camera);
            if (SourceReady != null) SourceReady(image);
        }

        void BindHook()
        {
            if (hookAttempted) return;
            hookAttempted = true;
            try
            {
                Type harmonyType = null, methodType = null;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if ((harmonyType = assembly.GetType("HarmonyLib.Harmony", false)) != null)
                    { methodType = assembly.GetType("HarmonyLib.HarmonyMethod", false); break; }
                if (harmonyType == null || methodType == null) return;
                MethodInfo original = type.GetMethod("EnableForThisFrame", BindingFlags.Instance | BindingFlags.Public);
                MethodInfo callback = typeof(OceanLayer).GetMethod("AfterOceanEnabled", BindingFlags.Static | BindingFlags.NonPublic);
                if (original == null) return;
                harmony = Activator.CreateInstance(harmonyType, new object[] { "koe.frame-generation.ocean-layer" });
                object postfix = Activator.CreateInstance(methodType, new object[] { callback });
                foreach (MethodInfo patch in harmonyType.GetMethods())
                {
                    if (patch.Name != "Patch") continue;
                    ParameterInfo[] parameters = patch.GetParameters();
                    if (parameters.Length == 0 || parameters[0].ParameterType != typeof(MethodBase)) continue;
                    object[] arguments = new object[parameters.Length];arguments[0] = original;
                    for (int i=1;i<parameters.Length;i++) if (parameters[i].Name == "postfix") arguments[i] = postfix;
                    hookOwner = this;
                    patch.Invoke(harmony, arguments);
                    Trace("hookInstalled",null);
                    break;
                }
            }
            catch (Exception error) { Debug.LogWarning("[KoeFrameGeneration] Ocean layer callback unavailable: " + error.Message); }
        }

        static void AfterOceanEnabled(Component __instance)
        {
            if (hookOwner != null && __instance != null)
            {
                Camera camera=__instance.GetComponent<Camera>();hookOwner.Trace("hookFired",camera);hookOwner.Observe(camera);
            }
        }

        void Trace(string action,Camera camera)
        {
            if(string.IsNullOrEmpty(traceRoot))return;
            string key=action+":"+(camera==null?0:camera.GetInstanceID());
            if(!traceEvents.Add(key))return;
            System.IO.File.AppendAllText(System.IO.Path.Combine(traceRoot,"ocean-source.jsonl"),
                "{\"action\":\""+action+"\",\"camera\":\""+(camera==null?"":camera.name.Replace("\"","\\\""))
                +"\",\"width\":"+(camera==null?0:camera.pixelWidth)+",\"height\":"+(camera==null?0:camera.pixelHeight)
                +",\"targetTexture\":"+(camera!=null && camera.targetTexture!=null?"true":"false")+"}\n");
        }

        public void Dispose()
        {
            Clear();
            if (hookOwner == this)
            {
                hookOwner = null;
                if (harmony != null)
                {
                    MethodInfo unpatch = harmony.GetType().GetMethod("UnpatchSelf");
                    if (unpatch != null) unpatch.Invoke(harmony, null);
                }
            }
        }

        public void Clear()
        {
            foreach (Capture capture in captures.Values)
            {
                if (capture.Camera != null) capture.Camera.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha, capture.Commands);
                capture.Commands.Dispose();
                if (capture.Image != null) { capture.Image.Release(); UnityEngine.Object.Destroy(capture.Image); }
            }
            captures.Clear(); frame = -1; image = IntPtr.Zero;
        }
    }
}
