using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Native Texture Encoder")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace NativeTextureFix
{
    public static class Pipeline
    {
        const string UnitySha = "e661981f033e2f1d32db6e44d43c0e0c1783b4cb3115caf393e7722673473317";
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Initialize(int mode);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Probe();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Counters([Out] long[] values);
        static Counters read;
        public static long Calls, ParallelCalls, Mismatches, Rejected;
        public static double SerialSeconds, ParallelSeconds;
        static string Hash(byte[] bytes)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
        }

        public static bool Apply()
        {
            if (read != null)
                return true;
            string mode = Environment.GetEnvironmentVariable("KOE_NATIVE_TEXTURE_MODE");
            if (!string.IsNullOrEmpty(mode) && mode != "0")
            {
                Debug.Log("[NativeTextureFix] Explicit diagnostic encoder mode overrides production initialization");
                return false;
            }

            string root = KSPUtil.ApplicationRootPath, actual;
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(Path.Combine(root, "UnityPlayer.dll")))
                actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
            if (actual != UnitySha)
                throw new NotSupportedException("Unsupported Unity native image");
            byte[] bytes;
            using (var source = Assembly.GetExecutingAssembly().GetManifestResourceStream("encoder.bin"))
            using (var output = new MemoryStream())
            {
                if (source == null)
                    throw new InvalidDataException("Missing embedded encoder");
                source.CopyTo(output);
                bytes = output.ToArray();
            }

            string hash = Hash(bytes), folder = Path.Combine(root, "KerbalOverEngineering-cache/native-encoder");
            Directory.CreateDirectory(folder);
            string path = Path.Combine(folder, hash + ".bin");
            if (File.Exists(path))
            {
                if (Hash(File.ReadAllBytes(path)) != hash)
                    throw new InvalidDataException("Native encoder cache identity mismatch");
            }
            else
            {
                string stage = path + ".new-" + Guid.NewGuid().ToString("N");
                try
                {
                    File.WriteAllBytes(stage, bytes);
                    try
                    {
                        File.Move(stage, path);
                    }
                    catch (IOException)
                    {
                        if (!File.Exists(path) || Hash(File.ReadAllBytes(path)) != hash)
                            throw;
                    }
                }
                finally
                {
                    if (File.Exists(stage))
                        File.Delete(stage);
                }
            }

            var module = LoadLibraryW(path);
            if (module == IntPtr.Zero)
                throw new InvalidOperationException("Native encoder load failed: " + Marshal.GetLastWin32Error());
            var start = GetProcAddress(module, "Initialize");
            var counter = GetProcAddress(module, "Counters");
            var probe = GetProcAddress(module, "ProbeEncoder");
            if (start == IntPtr.Zero || counter == IntPtr.Zero || probe == IntPtr.Zero)
                throw new MissingMethodException("Native encoder exports unavailable");
            if (((Probe)Marshal.GetDelegateForFunctionPointer(probe, typeof(Probe)))() != 1)
                throw new NotSupportedException("Native encoder not found uniquely in executable Unity sections");
            var initialize = (Initialize)Marshal.GetDelegateForFunctionPointer(start, typeof(Initialize));
            int workers = initialize(3);
            if (workers <= 0)
                throw new InvalidOperationException("Native encoder guard rejected: " + workers);
            read = (Counters)Marshal.GetDelegateForFunctionPointer(counter, typeof(Counters));
            Debug.Log("[NativeTextureFix] Loaded 1.0.0: original Unity block encoder; logicalProcessors=" + workers + "; mode=3");
            return true;
        }

        public static void Report()
        {
            if (read == null)
                return;
            var v = new long[9];
            read(v);
            Calls = v[0];
            ParallelCalls = v[1];
            Mismatches = v[3];
            Rejected = v[8];
            SerialSeconds = v[5] / (double)v[7];
            ParallelSeconds = v[6] / (double)v[7];
            Debug.Log(
                "[NativeTextureFix] Counters: calls=" + Calls
                + "; parallel=" + ParallelCalls
                + "; mismatches=" + Mismatches
                + "; rejected=" + Rejected
                + "; parallelSeconds=" + ParallelSeconds.ToString("F6", System.Globalization.CultureInfo.InvariantCulture)
            );
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.NativeTextureEncoder"))
            {
                Destroy(this);
                return;
            }

            try
            {
                if (!Pipeline.Apply())
                {
                    Destroy(this);
                    return;
                }

                DontDestroyOnLoad(gameObject);
                GameEvents.OnGameDatabaseLoaded.Add(Loaded);
            }
            catch (Exception ex)
            {
                Debug.LogError("[NativeTextureFix] Disabled: " + ex.Message);
                Destroy(this);
            }
        }

        public void Loaded()
        {
            Pipeline.Report();
            GameEvents.OnGameDatabaseLoaded.Remove(Loaded);
        }

        public void OnApplicationQuit()
        {
            Pipeline.Report();
        }
    }
}
