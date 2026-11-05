using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;

namespace DlcCacheFix
{
    public sealed class CacheStore
    {
        public readonly string Root, Engine;
        readonly object gate = new object ();
        sealed class Validated
        {
            public long Length, Stamp, ManifestStamp;
        }

        readonly Dictionary<string, Validated> validated = new Dictionary<string, Validated>();
        public CacheStore(string root, string engine)
        {
            Root = Path.GetFullPath(root);
            Engine = engine;
        }

        public static string HashFile(string path)
        {
            using (var hash = SHA256.Create())
            using (var input = File.OpenRead(path))
                return Hex(hash.ComputeHash(input));
        }

        public static string SourceKey(string path)
        {
            using (var hash = MD5.Create())
            using (var input = File.OpenRead(path))
                return Hex(hash.ComputeHash(input));
        }

        public static string Hex(byte[] bytes)
        {
            return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        }

        public string BundlePath(string key)
        {
            if (key == null || key.Length != 32)
                throw new ArgumentException("Invalid source key.");
            foreach (char c in key)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    throw new ArgumentException("Invalid source key.");
            return Path.Combine(Root, key + ".bundle");
        }

        public bool TryResolve(string key, out string path)
        {
            lock (gate)
                return Resolve(key, out path);
        }

        public int WarmExisting()
        {
            if (!Directory.Exists(Root))
                return 0;
            int count = 0;
            foreach (string manifest in Directory.GetFiles(Root, "*.bundle.manifest"))
            {
                string name = Path.GetFileName(manifest), path;
                string key = name.Substring(0, name.Length - ".bundle.manifest".Length);
                try
                {
                    if (TryResolve(key, out path))
                        count++;
                }
                catch (ArgumentException)
                { /* Unrelated file; never a usable source key. */
                }
            }

            return count;
        }

        bool Resolve(string key, out string path)
        {
            path = BundlePath(key);
            string manifest = path + ".manifest";
            try
            {
                var data = new FileInfo(path);
                var meta = new FileInfo(manifest);
                if (!data.Exists || !meta.Exists || meta.Length > 4096)
                    return false;
                Validated old;
                if (validated.TryGetValue(key, out old)
                    && old.Length == data.Length
                    && old.Stamp == data.LastWriteTimeUtc.Ticks
                    && old.ManifestStamp == meta.LastWriteTimeUtc.Ticks)
                    return true;
                string[] lines = File.ReadAllLines(manifest);
                long size;
                if (lines.Length != 5
                    || lines[0] != "KOE-DLC-LZ4-v1"
                    || lines[1] != Engine
                    || lines[2] != key
                    || !long.TryParse(lines[3], NumberStyles.None, CultureInfo.InvariantCulture, out size)
                    || size != data.Length
                    || lines[4].Length != 64
                    || HashFile(path) != lines[4])
                    return false;
                validated[key] = new Validated
                {
                    Length = data.Length,
                    Stamp = data.LastWriteTimeUtc.Ticks,
                    ManifestStamp = meta.LastWriteTimeUtc.Ticks
                };
                return true;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        public void Commit(string key, string temporary)
        {
            BundlePath(key);
            if (!string.Equals(Path.GetDirectoryName(Path.GetFullPath(temporary)), Root, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Cache staging path escaped root.");
            string sum = HashFile(temporary);
            long size = new FileInfo(temporary).Length;
            lock (gate)
                CommitLocked(key, temporary, sum, size);
        }

        void CommitLocked(string key, string temporary, string sum, long size)
        {
            string destination = BundlePath(key);
            Directory.CreateDirectory(Root);
            if (File.Exists(destination))
                File.Replace(temporary, destination, null);
            else
                File.Move(temporary, destination);
            string stage = destination + ".manifest-" + Guid.NewGuid().ToString("N");
            try
            {
                File.WriteAllLines(stage, new[] { "KOE-DLC-LZ4-v1", Engine, key, size.ToString(CultureInfo.InvariantCulture), sum });
                if (File.Exists(destination + ".manifest"))
                    File.Replace(stage, destination + ".manifest", null);
                else
                    File.Move(stage, destination + ".manifest");
            }
            finally
            {
                if (File.Exists(stage))
                    File.Delete(stage);
            }

            // AI-generated comment: The owned staging file was hashed before publication; do not read it again on the game thread.
            validated[key] = new Validated
            {
                Length = size,
                Stamp = File.GetLastWriteTimeUtc(destination).Ticks,
                ManifestStamp = File.GetLastWriteTimeUtc(destination + ".manifest").Ticks
            };
        }
    }
}
