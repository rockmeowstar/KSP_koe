using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace KoeCfDlcCache
{
    public sealed class GenerationCache : IDisposable
    {
        static readonly Regex GenerationName = new Regex("^[a-f0-9]{64}$", RegexOptions.CultureInvariant);
        static readonly Regex CacheFile = new Regex("^[a-f0-9]{32}\\.bundle(?:\\.manifest(?:-[a-f0-9]{32})?|\\.tmp-[a-f0-9]{32})?$", RegexOptions.CultureInvariant);
        readonly FileStream lease;
        public readonly string CurrentDirectory;
        public int RemovedGenerations { get; private set; }
        public long RemovedBytes { get; private set; }

        GenerationCache(string current, FileStream lease) { CurrentDirectory = current; this.lease = lease; }

        public static GenerationCache Open(string root, string generation)
        {
            if (!GenerationName.IsMatch(generation ?? "")) throw new ArgumentException("Invalid DLC cache generation");
            root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (Path.GetFileName(root) != "CF-DLC-LZ4-v1") throw new ArgumentException("Unexpected DLC generation root");
            CheckNoLinks(root);
            string current = Path.Combine(root, generation);
            CheckNoLinks(current);
            Directory.CreateDirectory(current);
            string lockPath = Path.Combine(current, ".lease");
            CheckNoLinks(lockPath);
            // Shared readers can use the same generation; cleanup requires an
            // exclusive handle and therefore cannot remove an active generation.
            var handle = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.Read, FileShare.Read);
            var cache = new GenerationCache(current, handle);
            try
            {
                Directory.SetLastWriteTimeUtc(current, DateTime.UtcNow);
                cache.Prune(root, generation);
                return cache;
            }
            catch { handle.Dispose(); throw; }
        }

        void Prune(string root, string current)
        {
            // Keep the current mod set and one recent rollback. Existing source
            // fingerprinting still invalidates content after any mod change.
            var older = new DirectoryInfo(root).GetDirectories()
                .Where(d => GenerationName.IsMatch(d.Name) && d.Name != current)
                .OrderByDescending(d => d.LastWriteTimeUtc).ToArray();
            foreach (var directory in older.Skip(1))
            {
                try
                {
                    string path = Path.GetFullPath(directory.FullName);
                    if (!string.Equals(Path.GetDirectoryName(path), root, StringComparison.OrdinalIgnoreCase)) continue;
                    CheckNoLinks(path);
                    if (directory.GetDirectories().Length != 0) continue;
                    var files = directory.GetFiles();
                    if (files.Any(f => (f.Attributes & FileAttributes.ReparsePoint) != 0
                        || f.Name != ".lease" && !CacheFile.IsMatch(f.Name))) continue;
                    string lockPath = Path.Combine(path, ".lease");
                    using (var exclusive = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
                    {
                        foreach (var file in files)
                        {
                            if (file.Name == ".lease") continue;
                            CheckNoLinks(file.FullName);
                            long bytes = file.Length;
                            File.Delete(file.FullName);
                            RemovedBytes += bytes;
                        }
                    }
                    File.Delete(lockPath);
                    Directory.Delete(path); // Never recursively traverse a cache directory.
                    RemovedGenerations++;
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        static void CheckNoLinks(string path)
        {
            for (string cursor = Path.GetFullPath(path); !string.IsNullOrEmpty(cursor); cursor = Path.GetDirectoryName(cursor))
                if ((File.Exists(cursor) || Directory.Exists(cursor)) && (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Cache cleanup does not follow links: " + cursor);
        }

        public void Dispose() { lease.Dispose(); }
    }
}
