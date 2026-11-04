using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;

namespace KerbalOverEngineering.Options
{
    static class HardwareProbe
    {
        [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet=CharSet.Ansi, ExactSpelling=true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Probe(uint vendor, uint device, uint width, uint height);
        public static string OpticalFlow()
        {
            try
            {
                string path=Path.Combine(NativeAssets.Extract(Assembly.GetExecutingAssembly()), "KoeHardwareProbe.dll");
                IntPtr module=LoadLibraryW(path);
                if(module==IntPtr.Zero) return "Hardware probe loader failed: " + Marshal.GetLastWin32Error();
                var probe=(Probe)Marshal.GetDelegateForFunctionPointer(GetProcAddress(module,"ProbeOpticalFlow"),typeof(Probe));
                int result=probe((uint)SystemInfo.graphicsDeviceVendorID,(uint)SystemInfo.graphicsDeviceID,(uint)Math.Max(1,Screen.width),(uint)Math.Max(1,Screen.height));
                return result==0 ? null : "Optical Flow capability/init rejected: " + result;
            }
            catch(Exception error) {return "Optical Flow probe failed: " + error.Message;}
        }
    }
}
