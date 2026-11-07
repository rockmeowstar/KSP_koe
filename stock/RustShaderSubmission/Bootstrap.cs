using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using KerbalOverEngineering.Options;
using UnityEngine;

namespace KerbalOverEngineering.ShaderSubmission
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
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Info([Out] ulong[] values, int count);
        Select select;

        public void Awake()
        {
            if (!Settings.Enabled("Rendering.RustShaderSubmission"))
            {
                Destroy(gameObject);
                return;
            }
            try
            {
                string directory = NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module = LoadLibraryW(Path.Combine(directory, "KoeShaderSubmission.dll"));
                if (module == IntPtr.Zero)
                    throw new InvalidOperationException("Shader submission native loader failed");
                var install = (Install)Marshal.GetDelegateForFunctionPointer(
                    GetProcAddress(module, "InstallShaderSubmission"), typeof(Install));
                select = (Select)Marshal.GetDelegateForFunctionPointer(
                    GetProcAddress(module, "SetShaderSubmissionMode"), typeof(Select));
                int result = install();
                if (result != 0 || select(2) != 0)
                    throw new InvalidOperationException("Shader submission initialization failed: " + result);
                var info = (Info)Marshal.GetDelegateForFunctionPointer(
                    GetProcAddress(module, "GetShaderSubmissionParallelInfo"), typeof(Info));
                ulong[] values = new ulong[7]; info(values, values.Length);
                Debug.Log("[KOE ShaderSubmission] Rust shader submission enabled; logical=" + values[0]
                    + "; workers=" + values[1] + "; parallelCopyThreshold=" + values[2]);
            }
            catch (Exception error)
            {
                if (select != null) select(0);
                Debug.LogError("[KOE ShaderSubmission] Original Unity path retained: " + error);
                Destroy(gameObject);
            }
        }

        public void OnDestroy() { if (select != null) select(0); }
        public void OnApplicationQuit() { if (select != null) select(0); }
    }
}
