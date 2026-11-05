using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace DlcCacheFix
{
    public static class ModFingerprint
    {
        static readonly string[] Generated =
        {
            "ModuleManager.ConfigCache",
            "ModuleManager.ConfigSHA",
            "ModuleManager.Physics",
            "ModuleManager.TechTree"
        };
        public static string Compute(string gameData, bool excludeRuntimeCaches = false)
        {
            string root = Path.GetFullPath(gameData).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            using (var hash = SHA256.Create())
            using (var stream = new CryptoStream(Stream.Null, hash, CryptoStreamMode.Write))
            using (var writer = new BinaryWriter(stream, Encoding.UTF8))
            {
                writer.Write("KOE-GameData-v2");
                if (Directory.Exists(root))
                    foreach (string file in Directory.GetFiles(root, "*", SearchOption.AllDirectories)
                        .OrderBy(p => p, StringComparer.Ordinal))
                    {
                        string relative = file.Substring(root.Length).Replace('\\', '/');
                        string extension = Path.GetExtension(file);
                        // AI-generated comment: Runtime hash sidecars are rewritten during planet loading;
                        // their timestamps do not identify an installed mod version.
                        if (excludeRuntimeCaches && string.Equals(extension, ".hash", StringComparison.OrdinalIgnoreCase))
                            continue;
                        if (Generated.Any(name => string.Equals(relative, name, StringComparison.OrdinalIgnoreCase))
                            || string.Equals(extension, ".log", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".tmp", StringComparison.OrdinalIgnoreCase))
                            continue;
                        var info = new FileInfo(file);
                        writer.Write(relative);
                        writer.Write(info.Length);
                        // AI-generated comment: Hash executable/configuration contents too, including replacements preserving timestamps.
                        if (string.Equals(extension, ".dll", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".cfg", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".version", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".xml", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".json", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".txt", StringComparison.OrdinalIgnoreCase)
                            || string.Equals(extension, ".ksp", StringComparison.OrdinalIgnoreCase))
                            writer.Write(CacheStore.HashFile(file));
                        else
                            writer.Write(info.LastWriteTimeUtc.Ticks);
                    }

                writer.Flush();
                stream.FlushFinalBlock();
                return CacheStore.Hex(hash.Hash);
            }
        }
    }
}
