using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using KerbalOverEngineering.Options;

namespace KerbalOverEngineering.ShaderConstants
{
    public sealed class Bootstrap : MonoBehaviour
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Install();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Select(int mode);
        Select select;

        public void Awake()
        {
            if (!Settings.Enabled("Rendering.ShaderConstantUpdates"))
            {
                Destroy(gameObject);
                return;
            }
            try
            {
                string folder = NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module = LoadLibraryW(Path.Combine(folder, "KoeUniformKernel.dll"));
                if (module == IntPtr.Zero)
                    throw new InvalidOperationException("Uniform native loader failed");
                var install = (Install)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "InstallUniformKernel"), typeof(Install));
                select = (Select)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module, "SetUniformKernelMode"), typeof(Select));
                int result = install();
                if (result != 0 || select(1) != 0)
                    throw new InvalidOperationException("Uniform native initialization failed: " + result);
                Debug.Log("[KOE ShaderConstants] SSE2 constant updates enabled");
            }
            catch (Exception error)
            {
                if (select != null) select(0);
                Debug.LogError("[KOE ShaderConstants] original Unity path retained: " + error);
                Destroy(gameObject);
            }
        }
        public void OnDestroy() { if (select != null) select(0); }
        public void OnApplicationQuit() { if (select != null) select(0); }
    }
}
