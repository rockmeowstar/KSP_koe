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
        Enable enable;
        public void Awake()
        {
            if (!Settings.Enabled("EVE.ShaderCorrectionGuards"))
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
                int result = initialize(Path.Combine(folder, "base-guarded.koepatch"), Path.Combine(folder, "top-guarded.koepatch"), 1);
                if (result != 0)
                    throw new InvalidOperationException("Native init: " + result);
                Debug.Log("[KOE CloudGuards] enabled; two exact shader identities; unknown shaders unchanged");
            }
            catch (Exception error)
            {
                if (enable != null)
                    enable(0);
                Debug.LogError("[KOE CloudGuards] disabled: " + error);
                Destroy(gameObject);
            }
        }

        public void OnApplicationQuit()
        {
            if (enable != null)
                enable(0);
        }
    }
}
