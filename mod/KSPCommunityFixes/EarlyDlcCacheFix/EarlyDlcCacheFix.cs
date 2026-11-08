using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Threading.Tasks;
using DlcCacheFix;
using HarmonyLib;
using KerbalOverEngineering.Options;
using UnityEngine;

namespace KoeCfDlcCache
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.KSPCF.EarlyDlcCache";
        sealed class Entry
        {
            internal string Source, Cached, Key;
            internal long Length, Stamp;
            internal bool Complete;
        }

        static readonly Dictionary<string, Entry> entries = new Dictionary<string, Entry>(
            Path.DirectorySeparatorChar == '\\' ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        static FieldInfo hash, bundle;
        static CacheStore store;
        static GenerationCache generation;
        static int hits, fallbacks, prepared, rejected;
        static bool running;
        int building;

        public void Awake()
        {
            if (!Settings.Enabled("KSPCommunityFixes.EarlyDlcCache")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                Type loader = AccessTools.TypeByName("KSPCommunityFixes.Performance.ExpansionBundlePreload");
                if (loader == null || loader.Module.ModuleVersionId != new Guid("62bb7f54-2099-4f40-9a34-377c091e8538"))
                    throw new NotSupportedException("The installed KSPCF DLC loader has not been reviewed.");
                Type info = loader.GetNestedType("AssetBundlePreloadInfo", BindingFlags.NonPublic);
                hash = AccessTools.Field(info, "hash");
                bundle = AccessTools.Field(info, "bundle");
                if (hash == null || hash.FieldType != typeof(byte[]) || bundle == null || bundle.FieldType != typeof(AssetBundle))
                    throw new MissingFieldException("KSPCF DLC result fields changed.");
                MethodInfo load = AccessTools.DeclaredMethod(loader, "LoadExpansionBundleAsyncV2");
                if (load == null || load.ReturnType != typeof(IEnumerator))
                    throw new MissingMethodException("KSPCF DLC load method changed.");
                var patches = Harmony.GetPatchInfo(load);
                if (patches != null && patches.Owners.Count > 0)
                    throw new NotSupportedException("Another mod patches KSPCF DLC bundle loading.");
                string root = Expansions.KSPExpansionsUtils.ExpansionsGameDataPath;
                if (Directory.Exists(root))
                    foreach (string directory in Directory.GetDirectories(root))
                    {
                        string assets = Path.Combine(directory, "AssetBundles");
                        if (!Directory.Exists(assets)) continue;
                        foreach (string source in Directory.GetFiles(assets))
                        {
                            var file = new FileInfo(source);
                            entries.Add(Path.GetFullPath(source), new Entry
                            {
                                Source = source, Length = file.Length, Stamp = file.LastWriteTimeUtc.Ticks
                            });
                        }
                    }
                harmony.Patch(load, postfix: new HarmonyMethod(typeof(Bootstrap), "Load"));
                running = true;
                StartCoroutine(Prepare());
            }
            catch (Exception error)
            {
                harmony.UnpatchAll(Id);
                Debug.LogWarning("[KOE EarlyDlcCache] Original loader retained: " + error.Message);
                Destroy(this);
            }
        }

        IEnumerator Prepare()
        {
            try
            {
                // AI-generated comment: KSPCF reads its MM-patched settings after configs load. Prepare before that barrier;
                // cached assets are consumed only if its own DLC coroutine actually runs.
                string root = KSPUtil.ApplicationRootPath, engine = Application.unityVersion;
                var fingerprint = Task.Factory.StartNew(() => ModFingerprint.Compute(Path.Combine(root, "GameData"), true));
                while (!fingerprint.IsCompleted) yield return null;
                if (fingerprint.IsFaulted)
                {
                    Debug.LogWarning("[KOE EarlyDlcCache] Mod fingerprint unavailable: " + fingerprint.Exception.GetBaseException().Message);
                    yield break;
                }
                var retention = Task.Factory.StartNew(() => GenerationCache.Open(Path.Combine(root, "KerbalOverEngineering-cache/CF-DLC-LZ4-v1"), fingerprint.Result));
                while (!retention.IsCompleted) yield return null;
                if (retention.IsFaulted)
                {
                    Debug.LogWarning("[KOE EarlyDlcCache] Cache ownership unavailable; original loader retained: " + retention.Exception.GetBaseException().Message);
                    yield break;
                }
                generation = retention.Result;
                if (generation.RemovedGenerations != 0)
                    Debug.Log("[KOE EarlyDlcCache] Removed " + generation.RemovedGenerations + " obsolete generations, " + generation.RemovedBytes + " bytes");
                store = new CacheStore(generation.CurrentDirectory,
                    engine + "|mods=" + fingerprint.Result);
                Debug.Log("[KOE EarlyDlcCache] Preparing " + entries.Count + " bundles; GameData=" + fingerprint.Result);
                int limit = Math.Max(1, Math.Min(Environment.ProcessorCount, entries.Count));
                foreach (Entry entry in entries.Values)
                {
                    while (building >= limit) yield return null;
                    building++;
                    StartCoroutine(BuildGuarded(entry));
                }
                while (building > 0) yield return null;
            }
            finally
            {
                foreach (Entry entry in entries.Values) entry.Complete = true;
                running = false;
            }
        }

        IEnumerator BuildGuarded(Entry entry)
        {
            var work = Build(entry);
            try
            {
                while (true)
                {
                    bool more;
                    try { more = work.MoveNext(); }
                    catch (Exception error)
                    {
                        rejected++;
                        Debug.LogWarning("[KOE EarlyDlcCache] Preparation skipped for " + Path.GetFileName(entry.Source) + ": " + error.Message);
                        break;
                    }
                    if (!more) break;
                    yield return work.Current;
                }
            }
            finally
            {
                var disposable = work as IDisposable;
                if (disposable != null) disposable.Dispose();
                entry.Complete = true;
                building--;
            }
        }

        static bool Unchanged(Entry entry)
        {
            var file = new FileInfo(entry.Source);
            return file.Exists && file.Length == entry.Length && file.LastWriteTimeUtc.Ticks == entry.Stamp;
        }

        static byte[] Digest(string source)
        {
            using (var md5 = MD5.Create())
            using (var input = File.OpenRead(source)) return md5.ComputeHash(input);
        }

        static IEnumerator Build(Entry entry)
        {
            string stage = null;
            try
            {
                var keyJob = Task.Factory.StartNew(() => CacheStore.SourceKey(entry.Source));
                while (!keyJob.IsCompleted) yield return null;
                if (keyJob.IsFaulted) throw keyJob.Exception.GetBaseException();
                entry.Key = keyJob.Result;
                if (!Unchanged(entry)) throw new IOException("DLC source changed before preparation.");
                var lookup = Task.Factory.StartNew(() =>
                {
                    string cached;
                    return store.TryResolve(entry.Key, out cached) ? cached : null;
                });
                while (!lookup.IsCompleted) yield return null;
                if (lookup.IsFaulted) throw lookup.Exception.GetBaseException();
                if (lookup.Result != null) { entry.Cached = lookup.Result; yield break; }
                Directory.CreateDirectory(store.Root);
                long reservation = 64L * 1024 * 1024;
                foreach (var item in entries.Values) reservation = checked(reservation + item.Length * 3);
                if (new DriveInfo(Path.GetPathRoot(store.Root)).AvailableFreeSpace < reservation)
                    throw new IOException("Insufficient free space for parallel DLC cache preparation.");
                stage = store.BundlePath(entry.Key) + ".tmp-" + Guid.NewGuid().ToString("N");
                var operation = AssetBundle.RecompressAssetBundleAsync(entry.Source, stage, BuildCompression.LZ4Runtime, 0, UnityEngine.ThreadPriority.High);
                while (!operation.isDone) yield return null;
                if (!operation.success) throw new IOException(operation.humanReadableResult);
                string output = stage;
                var commit = Task.Factory.StartNew(() =>
                {
                    if (!Unchanged(entry) || CacheStore.SourceKey(entry.Source) != entry.Key)
                        throw new IOException("DLC source changed during recompression.");
                    store.Commit(entry.Key, output);
                });
                while (!commit.IsCompleted) yield return null;
                if (commit.IsFaulted) throw commit.Exception.GetBaseException();
                entry.Cached = store.BundlePath(entry.Key);
                prepared++;
            }
            finally
            {
                if (stage != null)
                    try { if (File.Exists(stage)) File.Delete(stage); }
                    catch (IOException) { }
                    catch (UnauthorizedAccessException) { }
            }
        }

        static void Load(object info, string path, ref IEnumerator __result)
        {
            Entry entry;
            if (!entries.TryGetValue(Path.GetFullPath(path), out entry)) return;
            __result = LoadGuarded(info, entry, __result);
        }

        static IEnumerator LoadGuarded(object info, Entry entry, IEnumerator original)
        {
            bool loaded = false;
            var work = LoadCached(info, entry, () => loaded = true);
            try
            {
                while (true)
                {
                    bool more;
                    try { more = work.MoveNext(); }
                    catch (Exception error)
                    {
                        Debug.LogWarning("[KOE EarlyDlcCache] Original bundle fallback: " + error.Message);
                        break;
                    }
                    if (!more) break;
                    yield return work.Current;
                }
            }
            finally
            {
                var disposable = work as IDisposable;
                if (disposable != null) disposable.Dispose();
            }
            if (!loaded)
            {
                fallbacks++;
                yield return original;
            }
            else
            {
                var disposable = original as IDisposable;
                if (disposable != null) disposable.Dispose();
            }
        }

        static IEnumerator LoadCached(object info, Entry entry, Action success)
        {
            var digest = Task.Factory.StartNew(() => Digest(entry.Source));
            while ((running && !entry.Complete) || !digest.IsCompleted) yield return null;
            if (digest.IsFaulted) throw digest.Exception.GetBaseException();
            if (entry.Cached == null || !Unchanged(entry) || CacheStore.Hex(digest.Result) != entry.Key) yield break;
            var validation = Task.Factory.StartNew(() =>
            {
                string cached;
                return store.TryResolve(entry.Key, out cached) ? cached : null;
            });
            while (!validation.IsCompleted) yield return null;
            if (validation.IsFaulted) throw validation.Exception.GetBaseException();
            if (validation.Result == null) yield break;
            var request = AssetBundle.LoadFromFileAsync(validation.Result);
            yield return request;
            AssetBundle loaded = request.assetBundle;
            if (loaded == null) yield break;
            try
            {
                // AI-generated comment: KSPCF still verifies the signed original file's digest, never the LZ4 cache digest.
                hash.SetValue(info, digest.Result);
                bundle.SetValue(info, loaded);
            }
            catch { loaded.Unload(true); throw; }
            hits++;
            success();
        }

        public void OnApplicationQuit()
        {
            Debug.Log("[KOE EarlyDlcCache] hits=" + hits + "; prepared=" + prepared + "; rejected=" + rejected + "; originalFallback=" + fallbacks);
        }
    }
}
