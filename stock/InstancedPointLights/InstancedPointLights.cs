using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using KerbalOverEngineering.Options;

namespace KerbalOverEngineering.InstancedPointLights
{
    public sealed class Bootstrap : MonoBehaviour
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32", CharSet = CharSet.Unicode, ExactSpelling = true)]
        static extern IntPtr GetModuleHandleW(string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        delegate int Initialize(string folder, int mode);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Configure(int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Set(int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate IntPtr Pointer();
        Set scene, mode;
        IntPtr renderEvent;
        bool lastAllowed;
        static T Function<T>(IntPtr module, string name)
            where T : class
        {
            IntPtr pointer = GetProcAddress(module, name);
            if (pointer == IntPtr.Zero)
                throw new MissingMethodException(name);
            return Marshal.GetDelegateForFunctionPointer(pointer, typeof(T)) as T;
        }

        public void Awake()
        {
            if (!Settings.Enabled("Rendering.InstancedPointLights"))
            {
                Destroy(gameObject);
                return;
            }

            try
            {
                // AI-generated comment: RenderDoc wraps the D3D11 context; keep original
                // Unity draws instead of installing a second set of native hooks.
                if (GetModuleHandleW("renderdoc.dll") != IntPtr.Zero)
                {
                    Debug.Log("[KOE PointLights] RenderDoc detected; using original Unity lights for capture");
                    Destroy(gameObject);
                    return;
                }
                string folder = NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module = LoadLibraryW(Path.Combine(folder, "KoeInstancedLights.dll"));
                if (module == IntPtr.Zero)
                    throw new InvalidOperationException("Native loader: " + Marshal.GetLastWin32Error());
                bool normalize = Settings.Enabled("Rendering.BlendFactorNormalization");
                IntPtr configurePointer = GetProcAddress(module, "ConfigureBlendFactorNormalization");
                if (configurePointer != IntPtr.Zero)
                {
                    var configure = (Configure)Marshal.GetDelegateForFunctionPointer(configurePointer, typeof(Configure));
                    if (configure(normalize ? 1 : 0) != 0) throw new InvalidOperationException("Blend-factor configuration arrived after native initialization");
                }
                else if (normalize) Debug.LogWarning("[KOE PointLights] BlendFactorNormalization unavailable in this native adapter");
                scene = Function<Set>(module, "SetInstancedLightsScene");
                mode = Function<Set>(module, "SetInstancedLightsMode");
                renderEvent = Function<Pointer>(module, "GetInstancedLightsRenderEvent")();
                int result = Function<Initialize>(module, "InitializeInstancedLights")(folder, 1);
                if (result != 0)
                    throw new InvalidOperationException("Native init: " + result);
                Camera.onPostRender += PostRender;
                Debug.Log("[KOE PointLights] enabled; individual point sources; unknown shader/shadow/cookie paths unchanged");
            }
            catch (Exception error)
            {
                if (mode != null)
                    mode(0);
                Debug.LogError("[KOE PointLights] disabled: " + error);
                Destroy(gameObject);
            }
        }

        public void Update()
        {
            if (scene == null)
                return;
            bool allowed = HighLogic.LoadedSceneIsFlight
                || HighLogic.LoadedSceneIsEditor
                || HighLogic.LoadedScene == GameScenes.SPACECENTER;
            if (allowed != lastAllowed)
            {
                scene(allowed ? 1 : 0);
                lastAllowed = allowed;
            }
        }

        void PostRender(Camera camera)
        {
            if (renderEvent != IntPtr.Zero)
                GL.IssuePluginEvent(renderEvent, 1);
        }

        public void OnApplicationQuit()
        {
            if (mode != null)
                mode(0);
        }

        public void OnDestroy()
        {
            Camera.onPostRender -= PostRender;
            if (mode != null)
                mode(0);
        }
    }
}
