using System;
using System.Collections;
using System.IO;
using System.Collections.Generic;
using System.Threading.Tasks;
using UnityEngine;

namespace DlcCacheFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class CacheBuilder : MonoBehaviour
    {
        static CacheBuilder active;
        public static long Built, Skipped, Failed;
        public static bool Running;
        public static void BuildNow()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DlcEarlyPreparation")) return;
            if (active != null) return;
            Patch.Configure();
            var gameObject = new GameObject("KOE DLC cache builder");
            gameObject.AddComponent<CacheBuilder>();
            DontDestroyOnLoad(gameObject);
        }
        public void Start()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DlcEarlyPreparation")) { Destroy(gameObject); return; }
            if (active != null && active != this) { Destroy(gameObject); return; }
            active = this; Running = true; DontDestroyOnLoad(gameObject); StartCoroutine(GuardedBuild());
        }
        IEnumerator GuardedBuild()
        {
            var build = Build();
            try
            {
                while (true)
                {
                    bool more;
                    try { more = build.MoveNext(); }
                    catch (Exception ex) { Failed++; Debug.LogWarning("[DlcCacheFix] Cache preparation aborted: " + ex); break; }
                    if (!more) break;
                    yield return build.Current;
                }
            }
            finally { Running = false; active = null; Destroy(gameObject); }
        }
        IEnumerator Build()
        {
            Patch.Configure();
            while (!Patch.Preparation.IsCompleted) yield return null;
            if (Patch.Preparation.IsFaulted)
            {
                Failed++; Debug.LogWarning("[DlcCacheFix] Cache preparation unavailable: " + Patch.Preparation.Exception.GetBaseException().Message);
                yield break;
            }
            Debug.Log("[DlcCacheFix] GameData fingerprint: " + Patch.ModsFingerprint + "; verified existing=" + Patch.WarmedCaches);
            string expansion = Path.Combine(KSPUtil.ApplicationRootPath, "GameData/SquadExpansion");
            var files = new List<string>();
            if (Directory.Exists(expansion)) foreach (string directory in Directory.GetDirectories(expansion))
            {
                string bundles = Path.Combine(directory, "AssetBundles");
                if (Directory.Exists(bundles)) files.AddRange(Directory.GetFiles(bundles));
            }
            if (files.Count > 0 && Patch.RawLoads == 0 && Patch.CacheHits >= files.Count)
            { Skipped += files.Count; Debug.Log("[DlcCacheFix] All DLC bundles already came from verified cache."); yield break; }
            foreach (string source in files)
            {
                if (!string.Equals(Path.GetFileName(Path.GetDirectoryName(source)), "AssetBundles", StringComparison.OrdinalIgnoreCase)) continue;
                var keyJob = Task.Factory.StartNew(() => CacheStore.SourceKey(source));
                while (!keyJob.IsCompleted) yield return null;
                if (keyJob.IsFaulted) { Failed++; Debug.LogWarning("[DlcCacheFix] Source hash failed: " + keyJob.Exception.GetBaseException().Message); continue; }
                string key = keyJob.Result;
                var checkJob = Task.Factory.StartNew(() => { string path; return Patch.Store.TryResolve(key, out path); });
                while (!checkJob.IsCompleted) yield return null;
                if (!checkJob.IsFaulted && checkJob.Result) { Skipped++; continue; }
                string stage = null; AssetBundleRecompressOperation operation = null;
                try
                {
                    Directory.CreateDirectory(Patch.Store.Root);
                    if (new DriveInfo(Path.GetPathRoot(Patch.Store.Root)).AvailableFreeSpace < 2 * new FileInfo(source).Length + 64L * 1024 * 1024)
                        throw new IOException("Insufficient space for DLC cache staging.");
                    stage = Patch.Store.BundlePath(key) + ".tmp-" + Guid.NewGuid().ToString("N");
                    operation = AssetBundle.RecompressAssetBundleAsync(source, stage, BuildCompression.LZ4Runtime, 0, UnityEngine.ThreadPriority.Low);
                }
                catch (Exception ex) { Failed++; Debug.LogWarning("[DlcCacheFix] Cache build unavailable: " + ex.Message); }
                if (operation == null) { if (stage != null && File.Exists(stage)) File.Delete(stage); continue; }
                // Do not register a global AsyncOperation barrier: stock DLC may load in parallel.
                while (!operation.isDone) yield return null;
                if (!operation.success)
                {
                    Failed++; Debug.LogWarning("[DlcCacheFix] Recompression failed: " + operation.humanReadableResult);
                    if (File.Exists(stage)) File.Delete(stage); continue;
                }
                string output = stage;
                var commitJob = Task.Factory.StartNew(() =>
                {
                    if (CacheStore.SourceKey(source) != key) throw new IOException("DLC source changed during recompression.");
                    Patch.Store.Commit(key, output);
                });
                while (!commitJob.IsCompleted) yield return null;
                if (commitJob.IsFaulted)
                {
                    Failed++; Debug.LogWarning("[DlcCacheFix] Cache commit failed: " + commitJob.Exception.GetBaseException().Message);
                    if (File.Exists(stage)) File.Delete(stage);
                }
                else { Built++; Debug.Log("[DlcCacheFix] LZ4 cache ready: " + Path.GetFileName(source) + "; key=" + key); }
            }
            Debug.Log("[DlcCacheFix] Cache preparation finished: built=" + Built + "; existing=" + Skipped + "; failed=" + Failed);
        }
        public void OnDestroy() { if (active == this) { active = null; Running = false; } }
    }
}
