using System;
using System.IO;
using DlcCacheFix;

static class DlcCacheTests
{
    static int checks;
    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new Exception(message);
    }

    static void Main()
    {
        string root = Path.Combine(Path.GetTempPath(), "KOE-dlc-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            string data = Path.Combine(root, "GameData");
            Directory.CreateDirectory(data);
            string config = Path.Combine(data, "mod.cfg");
            File.WriteAllText(config, "value = 1");
            DateTime stamp = File.GetLastWriteTimeUtc(config);
            string first = ModFingerprint.Compute(data);
            File.WriteAllText(config, "value = 2");
            File.SetLastWriteTimeUtc(config, stamp);
            Check(first != ModFingerprint.Compute(data), "Same-size configuration replacement must invalidate the cache.");
            string changed = ModFingerprint.Compute(data);
            string cacheStamp = Path.Combine(data, "mesh.hash");
            File.WriteAllText(cacheStamp, "generated");
            Check(changed == ModFingerprint.Compute(data, true), "Runtime hash sidecars must not invalidate the CF DLC cache.");
            File.Delete(cacheStamp);
            File.WriteAllText(Path.Combine(data, "ModuleManager.ConfigCache"), "generated");
            Check(changed == ModFingerprint.Compute(data), "Generated MM cache must not invalidate the mod fingerprint.");
            string dll = Path.Combine(data, "new.dll");
            File.WriteAllBytes(dll, new byte[] { 1, 2, 3 });
            Check(changed != ModFingerprint.Compute(data), "Installing a mod must invalidate the cache.");
            File.Delete(dll);
            Check(changed == ModFingerprint.Compute(data), "Removing the added mod must recover the original fingerprint.");

            string source = Path.Combine(root, "original");
            File.WriteAllBytes(source, new byte[] { 1, 3, 5, 7 });
            string key = CacheStore.SourceKey(source);
            string cache = Path.Combine(root, "cache");
            Directory.CreateDirectory(cache);
            var store = new CacheStore(cache, "engine|mods=" + changed);
            string resolved;
            Check(!store.TryResolve(key, out resolved), "A missing bundle must use the original loader.");
            string stage = Path.Combine(cache, "owned.tmp");
            File.WriteAllBytes(stage, new byte[] { 2, 4, 6, 8 });
            store.Commit(key, stage);
            Check(store.TryResolve(key, out resolved) && File.ReadAllBytes(resolved)[0] == 2, "Committed cache must resolve.");
            Check(!new CacheStore(cache, "other-engine").TryResolve(key, out resolved), "Engine changes must invalidate the cache.");
            Check(!new CacheStore(cache, "engine|mods=other").TryResolve(key, out resolved), "Mod changes must invalidate the cache.");
            string bundle = store.BundlePath(key);
            File.WriteAllBytes(bundle, new byte[] { 8, 6, 4, 2 });
            Check(!new CacheStore(cache, store.Engine).TryResolve(key, out resolved), "Same-size corrupted bundle must be rejected after restart.");
            File.WriteAllText(bundle + ".manifest", "truncated");
            Check(!store.TryResolve(key, out resolved), "Truncated manifests must be rejected.");
            File.WriteAllBytes(source, new byte[] { 7, 5, 3, 1 });
            Check(key != CacheStore.SourceKey(source), "DLC replacement must change its source key.");
            bool escaped = false;
            try { store.Commit(key, source); } catch (ArgumentException) { escaped = true; }
            Check(escaped && File.Exists(source), "External files must never be moved into cache.");
            Console.WriteLine("DLC cache: " + checks + " invalidation, corruption and path-isolation checks passed.");
        }
        finally { Directory.Delete(root, true); }
    }
}
