using System;
using System.IO;
using KoeCfDlcCache;

static class CacheGenerationTests
{
    static void Check(bool value, string reason) { if (!value) throw new Exception(reason); }
    static string Name(int i) { return i.ToString("x64"); }
    static string Add(string root, int id)
    {
        string folder=Path.Combine(root,Name(id));Directory.CreateDirectory(folder);
        File.WriteAllBytes(Path.Combine(folder,new string('a',32)+".bundle"),new byte[1024]);
        Directory.SetLastWriteTimeUtc(folder,DateTime.UtcNow.AddMinutes(id-100));return folder;
    }
    static void Main()
    {
        string sandbox=Path.Combine(Path.GetTempPath(),"koe-cache-retention-"+Guid.NewGuid().ToString("N"));
        string root=Path.Combine(sandbox,"CF-DLC-LZ4-v1");Directory.CreateDirectory(root);
        try
        {
            for(int i=1;i<=20;i++)Add(root,i);
            string unknown=Path.Combine(root,"source-code");Directory.CreateDirectory(unknown);File.WriteAllText(Path.Combine(unknown,"keep.cs"),"source");
            string protectedGeneration=Add(root,21);File.WriteAllText(Path.Combine(protectedGeneration,"notes.txt"),"not generated cache");
            string nested=Add(root,22);Directory.CreateDirectory(Path.Combine(nested,"child"));
            string locked=Add(root,2);
            string currentData=Add(root,30);
            using(var active=new FileStream(Path.Combine(locked,".lease"),FileMode.OpenOrCreate,FileAccess.Read,FileShare.Read))
            using(var current=GenerationCache.Open(root,Name(30)))
            {
                Check(Directory.Exists(current.CurrentDirectory),"Current generation removed");
                Check(File.ReadAllBytes(Path.Combine(currentData,new string('a',32)+".bundle")).Length==1024,"Current bundle content changed");
                Check(Directory.Exists(locked),"Active generation removed");
                Check(Directory.Exists(protectedGeneration)&&File.Exists(Path.Combine(protectedGeneration,"notes.txt")),"Foreign file removed");
                Check(Directory.Exists(nested),"Nested directory traversed");
                Check(File.Exists(Path.Combine(unknown,"keep.cs")),"Non-generation folder removed");
                Check(current.RemovedGenerations>=17,"Obsolete cache versions accumulated");
                using(var same=GenerationCache.Open(root,Name(30)))Check(same.CurrentDirectory==current.CurrentDirectory,"Same generation cannot be shared");
            }
            // Releasing the active lease permits later cleanup.
            using(var next=GenerationCache.Open(root,Name(31)))Check(!Directory.Exists(locked),"Released obsolete generation retained forever");
            bool rejected=false;try{using(var bad=GenerationCache.Open(root,"../escape")) { }}catch(ArgumentException){rejected=true;}
            Check(rejected&&!Directory.Exists(Path.Combine(sandbox,"escape")),"Invalid generation escaped root");
            rejected=false;try{using(var bad=GenerationCache.Open(sandbox,Name(50))) { }}catch(ArgumentException){rejected=true;}
            Check(rejected,"Unexpected root accepted");
            Console.WriteLine("CACHE_RETENTION_PASS current_and_rollback_kept=True active_leases_preserved=True unknown_files_preserved=True no_recursive_delete=True invalid_paths_rejected=True");
        }
        finally { Directory.Delete(sandbox,true); }
    }
}
