using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using KerbalOverEngineering.Options;

namespace KerbalOverEngineering.CloudGuards
{
    public sealed class Bootstrap : MonoBehaviour
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        delegate int Initialize(string basePath, string topPath, int enabled);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Enable(int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Action();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate long Counter();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int InitializeFusion();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Read([Out] long[] values, int count);
        Enable enable;
        Enable packing;
        Enable mapPacking;
        Action clearMaps, shutdownMaps;
        Action clearFusion, shutdownFusion;
        Counter mapBinds;
        Read readMaps;
        bool sceneHandler, reportedMaps;
        long sceneBinds;
        public void Awake()
        {
            bool guardsEnabled = Settings.Enabled("EVE.ShaderCorrectionGuards");
            bool packingEnabled = Settings.Enabled("EVE.CloudShaderPacking") && SystemInfo.graphicsShaderLevel >= 50;
            bool mapsEnabled = Settings.Enabled("EVE.CloudMapPacking") && SystemInfo.graphicsShaderLevel >= 50;
            bool fusionEnabled = Settings.Enabled("EVE.CloudPassFusion") && packingEnabled;
            if (!guardsEnabled && !packingEnabled && !mapsEnabled)
            {
                Destroy(gameObject);
                return;
            }

            try
            {
                string folder = NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module = LoadLibraryW(Path.Combine(folder, "KoeCloudGuard.dll"));
                if (module == IntPtr.Zero)
                    throw new InvalidOperationException("Native loader: " + Marshal.GetLastWin32Error());
                var initialize = (Initialize)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "InitializeCloudGuards"), typeof(Initialize));
                enable = (Enable)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "SetCloudGuardsEnabled"), typeof(Enable));
                packing = (Enable)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "SetCloudPackingEnabled"), typeof(Enable));
                mapPacking = (Enable)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "SetCloudMapPackingEnabled"), typeof(Enable));
                clearMaps = (Action)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "ClearCloudMapCache"), typeof(Action));
                shutdownMaps = (Action)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "ShutdownCloudMaps"), typeof(Action));
                mapBinds = (Counter)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "GetCloudMapBindCount"), typeof(Counter));
                readMaps = (Read)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "ReadCloudMaps"), typeof(Read));
                packing(packingEnabled ? 1 : 0);
                mapPacking(mapsEnabled ? 1 : 0);
                int result = initialize(Path.Combine(folder, "base-guarded.koepatch"), Path.Combine(folder, "top-guarded.koepatch"), guardsEnabled ? 1 : 0);
                if (result != 0)
                    throw new InvalidOperationException("Native init: " + result);
                if (fusionEnabled)
                {
                    var initializeFusion = (InitializeFusion)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "InitializeCloudPassFusion"), typeof(InitializeFusion));
                    int fusionResult = initializeFusion();
                    if (fusionResult == 0)
                    {
                        clearFusion = (Action)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "ClearCloudPassFusion"), typeof(Action));
                        shutdownFusion = (Action)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "DisableCloudPassFusion"), typeof(Action));
                        Debug.Log("[KOE CloudFusion] enabled; runtime Rust compiler; unsupported passes use original draws");
                    }
                    else Debug.LogWarning("[KOE CloudFusion] using original passes; native init=" + fusionResult);
                }
                GameEvents.onGameSceneLoadRequested.Add(OnSceneLoad);
                sceneHandler = true;
                Debug.Log("[KOE CloudGuards] guards=" + guardsEnabled + "; Rust half packing=" + packingEnabled + "; Rust map packing=" + mapsEnabled);
                if (mapsEnabled) LogMaps("initialized");
            }
            catch (Exception error)
            {
                if (enable != null)
                    enable(0);
                if (packing != null)
                    packing(0);
                if (mapPacking != null) mapPacking(0);
                if (shutdownMaps != null) shutdownMaps();
                Debug.LogError("[KOE CloudGuards] disabled: " + error);
                Destroy(gameObject);
            }
        }

        public void OnApplicationQuit()
        {
            if (shutdownFusion != null) shutdownFusion();
            if (enable != null)
                enable(0);
            if (packing != null) packing(0);
            if (shutdownMaps != null) shutdownMaps();
        }

        public void Update()
        {
            if (!reportedMaps && mapBinds != null && mapBinds() > sceneBinds)
            {
                reportedMaps = true;
                LogMaps("active");
            }
        }

        void LogMaps(string phase)
        {
            if (readMaps == null) return;
            var values = new long[12];
            readMaps(values, values.Length);
            Debug.Log("[KOE CloudMaps] " + phase + "; enabled=" + values[1] + "; sources=" + values[2]
                + "; cpuBytes=" + values[3] + "; gpuBytes=" + values[4] + "; prepared=" + values[7]
                + "; binds=" + values[8] + "; fallbacks=" + values[9] + "; failures=" + values[11]);
        }

        void OnSceneLoad(GameScenes scene)
        {
            if (clearFusion != null) clearFusion();
            LogMaps("scene exit");
            if (clearMaps != null) clearMaps();
            if (mapBinds != null) sceneBinds = mapBinds();
            reportedMaps = false;
        }

        public void OnDestroy()
        {
            if (shutdownFusion != null) shutdownFusion();
            if (sceneHandler) GameEvents.onGameSceneLoadRequested.Remove(OnSceneLoad);
            if (shutdownMaps != null) shutdownMaps();
        }
    }
}
