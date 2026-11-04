using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace CloudLoopFix
{
    // An unordered, single-use bank. A miss never waits for an unrelated job or a full budget.
    // No persistent file data: every process reads the current mod assets again.
    public sealed class EarlyFileBank : IDisposable
    {
        sealed class Job
        { public string Path; public long Length, Stamp; public byte[] Data; public bool Taken; }
        readonly object gate=new object();
        readonly Dictionary<string,Job> jobs=new Dictionary<string,Job>(StringComparer.OrdinalIgnoreCase);
        readonly Queue<Job> queue=new Queue<Job>();
        readonly Thread[] workers;
        bool stopped;
        public readonly long ReservedBytes;
        public readonly int SelectedFiles;
        public long Hits, HitBytes;
        public EarlyFileBank(IEnumerable<string> paths,long budget,int workerCount)
        {
            if(budget<1||budget>int.MaxValue||workerCount<1||workerCount>4)throw new ArgumentOutOfRangeException();
            long reserved=0;
            foreach(var path in paths)
            {
                string full=Path.GetFullPath(path);if(jobs.ContainsKey(full))continue;
                try
                {
                    var info=new FileInfo(full);long length=info.Length;
                    if(length>budget-reserved)continue;
                    var job=new Job{Path=full,Length=length,Stamp=info.LastWriteTimeUtc.Ticks};
                    reserved+=length;jobs.Add(full,job);queue.Enqueue(job);
                }
                catch(IOException){}catch(UnauthorizedAccessException){}
            }
            ReservedBytes=reserved;SelectedFiles=jobs.Count;workers=new Thread[workerCount];
            try
            {
                for(int i=0;i<workers.Length;i++)
                {workers[i]=new Thread(Work){IsBackground=true,Name="KOE EVE file preparation"};workers[i].Start();}
            }
            catch{Dispose();throw;}
        }
        void Work()
        {
            while(true)
            {
                Job job;
                lock(gate){if(stopped||queue.Count==0)return;job=queue.Dequeue();if(job.Taken)continue;}
                byte[] data=null;
                try
                {
                    // A mod may replace the file after reservation. Never allocate its new, unbounded size.
                    using(var stream=new FileStream(job.Path,FileMode.Open,FileAccess.Read,FileShare.Read))
                    {
                        if(stream.Length!=job.Length)continue;
                        data=new byte[checked((int)job.Length)];int offset=0;
                        while(offset<data.Length)
                        {int read=stream.Read(data,offset,data.Length-offset);if(read==0)throw new EndOfStreamException();offset+=read;}
                    }
                    var info=new FileInfo(job.Path);
                    if(data.LongLength!=job.Length||info.Length!=job.Length||info.LastWriteTimeUtc.Ticks!=job.Stamp)data=null;
                }
                catch(IOException){}catch(UnauthorizedAccessException){}
                lock(gate){if(!stopped&&!job.Taken)job.Data=data;}
            }
        }
        public bool TryTake(string path,out byte[] data)
        {
            data=null;
            lock(gate)
            {
                Job job;if(stopped||!jobs.TryGetValue(Path.GetFullPath(path),out job)||job.Taken)return false;
                job.Taken=true;
                try
                {
                    var info=new FileInfo(job.Path);
                    if(job.Data!=null&&info.Length==job.Length&&info.LastWriteTimeUtc.Ticks==job.Stamp)data=job.Data;
                }
                catch(IOException){}catch(UnauthorizedAccessException){}
                job.Data=null;
                if(data==null)return false;
                Hits++;HitBytes+=data.LongLength;return true;
            }
        }
        public void Dispose()
        {lock(gate){stopped=true;queue.Clear();foreach(var job in jobs.Values){job.Taken=true;job.Data=null;}}}
        public bool WaitForWorkers(int timeoutMs)
        {
            DateTime until=DateTime.UtcNow.AddMilliseconds(timeoutMs);
            foreach(var worker in workers)
                if(worker!=null&&worker.IsAlive&&!worker.Join(Math.Max(0,(int)(until-DateTime.UtcNow).TotalMilliseconds)))return false;
            return true;
        }
    }
}
