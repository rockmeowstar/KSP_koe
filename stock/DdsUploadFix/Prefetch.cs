using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace DdsUploadFix
{
    // AI-generated comment: Bounded file I/O only. No Unity object, decoder or GPU calls on these threads.
    public sealed class FilePrefetch : IDisposable
    {
        sealed class Job
        {
            public int Index;
            public string Path;
            public long Length, Stamp, Reservation;
            public byte[] Bytes;
            public bool Done, Taken;
        }

        readonly object gate = new object ();
        readonly List<Job> jobs = new List<Job>();
        readonly Dictionary<string, Job> lookup = new Dictionary<string, Job>(StringComparer.OrdinalIgnoreCase);
        readonly Thread[] workers;
        readonly long budget;
        int next, consumedBefore;
        long reserved;
        bool stopped;
        public long MaxReservedBytes, PrefetchedFiles, PrefetchedBytes, SynchronousReads, DiscardedBytes, DiskReads;
        public FilePrefetch(IEnumerable<string> paths, long budgetBytes, int workerCount)
        {
            if (budgetBytes <= 0 || workerCount < 1)
                throw new ArgumentOutOfRangeException();
            budget = budgetBytes;
            foreach (string path in paths)
            {
                string full = System.IO.Path.GetFullPath(path);
                if (lookup.ContainsKey(full))
                    continue;
                var job = new Job
                {
                    Path = full,
                    Index = jobs.Count
                };
                lookup.Add(full, job);
                jobs.Add(job);
            }

            workers = new Thread[Math.Min(workerCount, jobs.Count)];
            try
            {
                for (int i = 0; i < workers.Length; i++)
                {
                    workers[i] = new Thread(Work)
                    {
                        IsBackground = true,
                        Name = "KOE DDS I/O " + i
                    };
                    workers[i].Start();
                }
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        void Work()
        {
            while (true)
            {
                Job job;
                lock (gate)
                {
                    if (stopped || next >= jobs.Count)
                        return;
                    job = jobs[next];
                    if (job.Taken)
                    {
                        job.Done = true;
                        next++;
                        Monitor.PulseAll(gate);
                        continue;
                    }

                    try
                    {
                        var info = new FileInfo(job.Path);
                        job.Length = info.Length;
                        job.Stamp = info.LastWriteTimeUtc.Ticks;
                    }
                    catch
                    {
                        job.Done = true;
                        next++;
                        Monitor.PulseAll(gate);
                        continue;
                    }

                    if (job.Length > budget)
                    {
                        job.Done = true;
                        next++;
                        Monitor.PulseAll(gate);
                        continue;
                    }

                    // AI-generated comment: Reserve in input order: later files cannot occupy the budget ahead of an earlier blocked job.
                    if (reserved + job.Length > budget)
                    {
                        Monitor.Wait(gate);
                        continue;
                    }

                    next++;
                    job.Reservation = job.Length;
                    reserved += job.Reservation;
                    MaxReservedBytes = Math.Max(MaxReservedBytes, reserved);
                }

                byte[] bytes = null;
                try
                {
                    lock (gate)
                        DiskReads++;
                    bytes = File.ReadAllBytes(job.Path);
                    var info = new FileInfo(job.Path);
                    if (bytes.LongLength != job.Length || info.Length != job.Length || info.LastWriteTimeUtc.Ticks != job.Stamp)
                        bytes = null;
                }
                catch
                {
                } // Original, current error is raised only when the main loader actually requests this file.

                lock (gate)
                {
                    job.Done = true;
                    if (!stopped && !job.Taken)
                        job.Bytes = bytes;
                    else
                    {
                        reserved -= job.Reservation;
                        job.Reservation = 0;
                    }

                    Monitor.PulseAll(gate);
                }
            }
        }

        public byte[] Read(string path)
        {
            Job job;
            byte[] result = null;
            lock (gate)
            {
                if (!stopped && lookup.TryGetValue(System.IO.Path.GetFullPath(path), out job) && !job.Taken)
                {
                    // AI-generated comment: A previous loader/plugin may skip files. Release skipped earlier reservations to avoid a full queue.
                    while (consumedBefore < job.Index)
                    {
                        Job earlier = jobs[consumedBefore++];
                        if (earlier.Taken)
                            continue;
                        earlier.Taken = true;
                        if (earlier.Bytes != null)
                        {
                            DiscardedBytes += earlier.Bytes.LongLength;
                            earlier.Bytes = null;
                        }

                        if (earlier.Done)
                        {
                            reserved -= earlier.Reservation;
                            earlier.Reservation = 0;
                        }
                    }

                    Monitor.PulseAll(gate);
                    while (!stopped && !job.Done)
                        Monitor.Wait(gate);
                    if (job.Done)
                    {
                        result = job.Bytes;
                        job.Bytes = null;
                        job.Taken = true;
                        reserved -= job.Reservation;
                        job.Reservation = 0;
                        Monitor.PulseAll(gate);
                    }

                    if (result != null)
                    {
                        try
                        {
                            var info = new FileInfo(path);
                            if (info.Length != job.Length || info.LastWriteTimeUtc.Ticks != job.Stamp)
                            {
                                DiscardedBytes += result.LongLength;
                                result = null;
                            }
                        }
                        catch
                        {
                            DiscardedBytes += result.LongLength;
                            result = null;
                        }
                    }
                }

                if (result != null)
                {
                    PrefetchedFiles++;
                    PrefetchedBytes += result.LongLength;
                }
                else
                {
                    SynchronousReads++;
                    DiskReads++;
                }
            }

            return result ?? File.ReadAllBytes(path);
        }

        public void Dispose()
        {
            lock (gate)
            {
                stopped = true;
                foreach (Job job in jobs)
                {
                    if (job.Bytes != null)
                    {
                        DiscardedBytes += job.Bytes.LongLength;
                        job.Bytes = null;
                    }

                    if (job.Done)
                    {
                        reserved -= job.Reservation;
                        job.Reservation = 0;
                    }
                }

                Monitor.PulseAll(gate);
            }
        }

        public bool WaitForWorkers(int timeoutMs)
        {
            DateTime end = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            foreach (Thread thread in workers)
                if (thread != null && thread.IsAlive && !thread.Join(Math.Max(0, (int)(end - DateTime.UtcNow).TotalMilliseconds)))
                    return false;
            return true;
        }
    }

    public static class PrefetchPhase
    {
        sealed class CompletionListener
        {
            public void End()
            {
                Complete();
            }
        }

        static readonly CompletionListener listener = new CompletionListener();
        // AI-generated comment: KSP EventVoid dereferences delegate.Target; static callbacks are not accepted.
        public static readonly EventVoid.OnEvent CompletionCallback = listener.End;
        static FilePrefetch active;
        static bool registered, attempted;
        public static long PrefetchedFiles, PrefetchedBytes, SynchronousReads, DiscardedBytes, MaxReservedBytes;
        public static void Register()
        {
            if (registered)
                return;
            GameEvents.OnGameDatabaseLoaded.Add(CompletionCallback);
            UnityEngine.Application.quitting += Complete;
            registered = true;
        }

        public static byte[] Read(string path)
        {
            if (!attempted)
            {
                attempted = true;
                try
                {
                    // AI-generated comment: KSP starts addons inside LoadObjects; its factory/driver already ran.
                    // Begin at the first actual DDS read and stop on the database-complete event.
                    var database = GameDatabase.Instance;
                    if (database != null && !database.IsReady())
                    {
                        var paths = new List<string>();
                        foreach (var file in database.root.GetFiles(UrlDir.FileType.Texture))
                            if (string.Equals(file.fileExtension, "dds", StringComparison.OrdinalIgnoreCase))
                                paths.Add(file.fullPath);
                        var resources = KerbalOverEngineering.Options.LoadingResources.Current;
                        if (resources.DdsBudget > 0)
                            active = new FilePrefetch(paths, resources.DdsBudget, resources.Workers);
                    }
                }
                catch (Exception ex)
                {
                    UnityEngine.Debug.LogWarning("[DdsUploadFix] Prefetch unavailable; using stock file reads: " + ex.Message);
                }
            }

            try
            {
                return active == null ? File.ReadAllBytes(path) : active.Read(path);
            }
            catch
            {
                Complete();
                throw;
            }
        }

        public static void Complete()
        {
            var phase = active;
            active = null;
            if (phase == null)
                return;
            phase.Dispose();
            PrefetchedFiles = phase.PrefetchedFiles;
            PrefetchedBytes = phase.PrefetchedBytes;
            SynchronousReads = phase.SynchronousReads;
            DiscardedBytes = phase.DiscardedBytes;
            MaxReservedBytes = phase.MaxReservedBytes;
            UnityEngine.Debug.Log(
                "[DdsUploadFix] I/O complete: prefetchedFiles=" + PrefetchedFiles
                + "; prefetchedBytes=" + PrefetchedBytes
                + "; syncReads=" + SynchronousReads
                + "; discardedBytes=" + DiscardedBytes
                + "; maxReservedBytes=" + MaxReservedBytes
                + "; avoidedPayloadCopies=" + Patch.AvoidedCopies
                + "; avoidedPayloadBytes=" + Patch.AvoidedCopyBytes
            );
        }
    }
}
