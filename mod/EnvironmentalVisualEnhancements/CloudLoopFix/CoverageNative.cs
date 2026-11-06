using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace CloudLoopFix
{
    public static class CoverageNative
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Version();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int BuildPrefix(IntPtr input, UIntPtr size, uint width, uint height, IntPtr output, UIntPtr entries);
        static readonly BuildPrefix build;
        static CoverageNative() { build = Load(); }
        public static bool Available { get { return build != null; } }

        static BuildPrefix Load()
        {
            if (Environment.OSVersion.Platform != PlatformID.Win32NT || IntPtr.Size != 8) return null;
            try
            {
                string folder = KerbalOverEngineering.Options.NativeAssets.Extract(Assembly.GetExecutingAssembly());
                IntPtr module = LoadLibraryW(Path.Combine(folder, "KoeCoverageKernel.dll"));
                if (module == IntPtr.Zero) return null;
                IntPtr version = GetProcAddress(module, "CoverageKernelVersion");
                IntPtr entry = GetProcAddress(module, "BuildCoveragePrefix");
                if (version == IntPtr.Zero || entry == IntPtr.Zero
                    || ((Version)Marshal.GetDelegateForFunctionPointer(version, typeof(Version)))() != 1) return null;
                return (BuildPrefix)Marshal.GetDelegateForFunctionPointer(entry, typeof(BuildPrefix));
            }
            catch (Exception) { return null; }
        }

        public static unsafe bool TryBuild(byte[] bytes, int offset, int width, int height, int[] output)
        {
            if (build == null) return false;
            if (bytes == null || output == null || offset < 0 || width <= 0 || height <= 0
                || bytes.LongLength - offset != (long)width * height * 8
                || output.LongLength != ((long)width + 1) * ((long)height + 1)) return false;
            fixed (byte* input = bytes)
            fixed (int* result = output)
                return build((IntPtr)(input + offset), (UIntPtr)(uint)(bytes.Length - offset), (uint)width, (uint)height,
                    (IntPtr)result, (UIntPtr)(uint)output.Length) == 0;
        }
    }
}
