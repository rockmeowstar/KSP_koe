using System;
using System.Runtime.InteropServices;
namespace KerbalOverEngineering.Options
{
    public static class RuntimeCompatibility
    {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
        [DllImport("kernel32.dll", CharSet=CharSet.Ansi, ExactSpelling=true)] static extern IntPtr GetProcAddress(IntPtr module,string name);
        static bool? wine;
        public static bool IsWine
        {
            get
            {
                if(wine.HasValue)return wine.Value;
                bool result=false;
                if(Environment.OSVersion.Platform==PlatformID.Win32NT)
                    try {var module=GetModuleHandleW("ntdll.dll");result=module!=IntPtr.Zero&&GetProcAddress(module,"wine_get_version")!=IntPtr.Zero;} catch { }
                wine=result;return result;
            }
        }
        public static string Description { get { return IsWine ? "Wine/Proton experimental; individual API probes required" : Environment.OSVersion.Platform.ToString(); } }
    }
}
