using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Networking;
using Debug = UnityEngine.Debug;

[assembly: AssemblyTitle("KerbalOverEngineering PNG Pipeline")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace PngPipelineFix
{
    public static class Pipeline
    {
        sealed class Job
        {
            public string Path, Url;
            public int Index;
            public long Stamp, Length, Bytes;
            public bool Claimed, Finished;
            public UnityWebRequest Request;
            public UnityWebRequestAsyncOperation Operation;
        }

        static readonly long Budget = KerbalOverEngineering.Options.LoadingResources.Current.PngBudget;
        static readonly List<Job> jobs = new List<Job>();
        static readonly Dictionary<string, Job> lookup = new Dictionary<string, Job>(StringComparer.OrdinalIgnoreCase);
        static readonly Dictionary<UnityWebRequest, Job> sent = new Dictionary<UnityWebRequest, Job>();
        static bool initialized, stopped;
        static int next, residents, consumedBefore;
        static long reserved;
        public static readonly int Window = KerbalOverEngineering.Options.LoadingResources.Current.DecodeWindow;
        public static readonly long QueueWindow = Window;
        public static long Uses, ReadyAtUse, Started, Fallback, PeakResident, PeakReserved;
        static void Init()
        {
            if (initialized)
                return;
            initialized = true;
            foreach (var file in GameDatabase.Instance.root.GetFiles(UrlDir.FileType.Texture))
            {
                if (!string.Equals(file.fileExtension, "png", StringComparison.OrdinalIgnoreCase))
                    continue;
                string path = Path.GetFullPath(file.fullPath), url = KSPUtil.ApplicationFileProtocol + path;
                if (lookup.ContainsKey(url))
                    continue;
                var job = new Job
                {
                    Path = path,
                    Url = url,
                    Index = jobs.Count
                };
                jobs.Add(job);
                lookup.Add(url, job);
            }
        }

        public static long EstimateFile(string path, out long length, out long stamp)
        {
            var info = new FileInfo(path);
            length = info.Length;
            stamp = info.LastWriteTimeUtc.Ticks;
            var header = new byte[24];
            using (var input = File.OpenRead(path))
                if (input.Read(header, 0, header.Length) != header.Length)
                    return 0;
            byte[] signature =
            {
                137,
                80,
                78,
                71,
                13,
                10,
                26,
                10
            };
            for (int i = 0; i < signature.Length; i++)
                if (header[i] != signature[i])
                    return 0;
            if (header[8] != 0
                || header[9] != 0
                || header[10] != 0
                || header[11] != 13
                || header[12] != 73
                || header[13] != 72
                || header[14] != 68
                || header[15] != 82)
                return 0;
            long w = 0, h = 0;
            for (int i = 16; i < 20; i++)
                w = (w << 8) | header[i];
            for (int i = 20; i < 24; i++)
                h = (h << 8) | header[i];
            if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
                return 0;
            return checked(w * h * 16 + length); // decoded CPU/GPU copies and mipmaps, conservative estimate.
        }

        static void Fill()
        {
            if (stopped || Window == 0)
                return;
            while (next < jobs.Count && residents < Window)
            {
                var job = jobs[next];
                if (job.Finished)
                {
                    next++;
                    continue;
                }

                try
                {
                    if (job.Bytes == 0)
                        job.Bytes = EstimateFile(job.Path, out job.Length, out job.Stamp);
                    if (job.Bytes <= 0 || job.Bytes > Budget)
                    {
                        next++;
                        continue;
                    }

                    if (reserved + job.Bytes > Budget)
                        break;
                    job.Request = UnityWebRequestTexture.GetTexture(job.Url);
                    job.Operation = job.Request.SendWebRequest();
                    sent.Add(job.Request, job);
                    residents++;
                    reserved += job.Bytes;
                    Started++;
                    next++;
                    PeakResident = Math.Max(PeakResident, residents);
                    PeakReserved = Math.Max(PeakReserved, reserved);
                }
                catch (Exception ex)
                {
                    if (job.Request != null)
                        job.Request.Dispose();
                    job.Request = null;
                    job.Operation = null;
                    next++;
                    Debug.LogWarning("[PngPipelineFix] Prefetch fallback: " + ex.Message);
                }
            }
        }

        static void Drop(Job job, bool consumed)
        {
            if (job.Request != null)
            {
                sent.Remove(job.Request);
                residents--;
                reserved -= job.Bytes;
                if (!consumed)
                {
                    if (job.Request.isDone && job.Request.error == null)
                    {
                        var texture = DownloadHandlerTexture.GetContent(job.Request);
                        if (texture != null)
                            UnityEngine.Object.Destroy(texture);
                    }
                    else
                        job.Request.Abort();
                    job.Request.Dispose();
                }

                // AI-generated comment: Consumed requests keep exactly the stock ownership/lifetime; do not destroy registered textures.
                job.Request = null;
                job.Operation = null;
            }

            job.Finished = true;
        }

        public static UnityWebRequest GetTexture(string url)
        {
            if (Window > 0 && !stopped)
            {
                Init();
                Job job;
                if (lookup.TryGetValue(url, out job) && !job.Finished)
                {
                    // AI-generated comment: Release skipped earlier files, preserving the bounded queue even with an asset blacklist.
                    while (consumedBefore < job.Index)
                    {
                        var earlier = jobs[consumedBefore++];
                        if (!earlier.Claimed && !earlier.Finished)
                            Drop(earlier, false);
                    }

                    Fill();
                    if (job.Request != null)
                    {
                        var info = new FileInfo(job.Path);
                        if (info.Length == job.Length && info.LastWriteTimeUtc.Ticks == job.Stamp)
                        {
                            job.Claimed = true;
                            Uses++;
                            if (job.Operation.isDone)
                                ReadyAtUse++;
                            return job.Request;
                        }

                        Drop(job, false);
                    }
                }
            }

            Fallback++;
            return UnityWebRequestTexture.GetTexture(url);
        }

        public static UnityWebRequestAsyncOperation Send(UnityWebRequest request)
        {
            Job job;
            return sent.TryGetValue(request, out job) ? job.Operation : request.SendWebRequest();
        }

        public static void Wrap(ref IEnumerator __result, FileInfo file)
        {
            if (!stopped)
                __result = Run(__result, file.FullName);
        }

        static IEnumerator Run(IEnumerator original, string path)
        {
            try
            {
                while (original.MoveNext())
                    yield return original.Current;
            }
            finally
            {
                var disposable = original as IDisposable;
                if (disposable != null)
                    disposable.Dispose();
                Job job;
                if (lookup.TryGetValue(KSPUtil.ApplicationFileProtocol + path, out job) && job.Claimed)
                    Drop(job, true);
                Fill();
            }
        }

        public static void Stop()
        {
            if (stopped)
                return;
            stopped = true;
            foreach (var job in jobs)
                if (job.Request != null && !job.Claimed)
                    Drop(job, false);
            jobs.Clear();
            lookup.Clear();
            sent.Clear();
            Debug.Log(
                "[PngPipelineFix] Complete: window=" + Window
                + "; used=" + Uses
                + "; readyAtUse=" + ReadyAtUse
                + "; started=" + Started
                + "; fallback=" + Fallback
                + "; peakResident=" + PeakResident
                + "; peakReserved=" + PeakReserved
            );
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            var pairs = new[]
            {
                new[]
                {
                    typeof(UnityWebRequestTexture).GetMethod("GetTexture", new[] { typeof(string) }),
                    typeof(Pipeline).GetMethod("GetTexture")
                },
                new[]
                {
                    typeof(UnityWebRequest).GetMethod("SendWebRequest", Type.EmptyTypes),
                    typeof(Pipeline).GetMethod("Send")
                }
            };
            int[] expected =
            {
                1,
                1
            };
            for (int p = 0; p < pairs.Length; p++)
            {
                int count = 0;
                foreach (var instruction in code)
                    if (instruction.Calls(pairs[p][0]))
                    {
                        instruction.opcode = OpCodes.Call;
                        instruction.operand = pairs[p][1];
                        count++;
                    }

                if (count != expected[p])
                    throw new NotSupportedException("The PNG loader no longer calls the expected method: " + p + " count=" + count);
            }

            return code;
        }
    }

    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.png-pipeline";
        static Harmony harmony;
        sealed class CompletionListener
        {
            public void End()
            {
                Pipeline.Stop();
            }
        }

        static readonly CompletionListener listener = new CompletionListener();
        public static readonly EventVoid.OnEvent CompletionCallback = listener.End;
        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.PngPipeline"))
                return;
            if (harmony != null)
                return;
            if (typeof(DatabaseLoaderTexture_PNG).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                throw new NotSupportedException("Unsupported KSP PNG loader.");
            var patcher = new Harmony(Id);
            try
            {
                var move = typeof(DatabaseLoaderTexture_PNG).GetNestedType("<Load>d__0", BindingFlags.NonPublic).GetMethod("MoveNext", BindingFlags.NonPublic | BindingFlags.Instance);
                patcher.Patch(move, null, null, new HarmonyMethod(typeof(Pipeline), "Transpile"));
                patcher.Patch(AccessTools.DeclaredMethod(typeof(DatabaseLoaderTexture_PNG), "Load"), null, new HarmonyMethod(typeof(Pipeline), "Wrap"));
                GameEvents.OnGameDatabaseLoaded.Add(CompletionCallback);
                Application.quitting += Pipeline.Stop;
            }
            catch
            {
                patcher.UnpatchAll(Id);
                throw;
            }

            harmony = patcher;
            Debug.Log("[PngPipelineFix] Loaded: window=" + Pipeline.Window + "; hardware-sized queue budget; stock conversion retained.");
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try
            {
                Patch.Apply();
            }
            catch (Exception ex)
            {
                Debug.LogError("[PngPipelineFix] Disabled: " + ex);
            }

            Destroy(this);
        }
    }
}

