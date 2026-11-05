using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using System.Threading.Tasks;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering DLC LZ4 Cache")]
[assembly: AssemblyVersion("1.0.0.0")]
namespace DlcCacheFix
{
    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.dlc-lz4-cache";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static readonly MethodInfo Hash = typeof(HashAlgorithm).GetMethod("ComputeHash", new[] { typeof(byte[]) });
        static readonly MethodInfo Load = typeof(AssetBundle).GetMethod("LoadFromMemory", new[] { typeof(byte[]) });
        static Harmony harmony;
        [ThreadStatic]
        static byte[] digest;
        public static CacheStore Store;
        public static Task Preparation;
        public static string ModsFingerprint;
        public static long CacheHits, RawLoads, RejectedCaches, HashTicks, ValidationTicks, CacheLoadTicks, RawLoadTicks;
        public static readonly long TimerFrequency = Stopwatch.Frequency;
        public static long WarmedCaches, WarmupTicks, WarmupFailures;
        public static void Configure()
        {
            if (Preparation != null)
                return;
            string root = KSPUtil.ApplicationRootPath, engine = Application.unityVersion;
            Preparation = Task.Factory.StartNew(() =>
            {
                long start = Stopwatch.GetTimestamp();
                try
                {
                    ModsFingerprint = ModFingerprint.Compute(Path.Combine(root, "GameData"));
                    Store = new CacheStore(Path.Combine(root, "KerbalOverEngineering-cache/DLC-LZ4-v2"), engine
                        + "|mods=" + ModsFingerprint);
                    WarmedCaches = Store.WarmExisting();
                }
                catch (Exception)
                {
                    WarmupFailures++;
                    throw;
                }
                finally
                {
                    WarmupTicks = Stopwatch.GetTimestamp() - start;
                }
            });
        }

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DlcLz4Cache"))
                return;
            if (harmony != null)
                return;
            if (typeof(ModuleEngines).Assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("Unsupported KSP DLC loader.");
            Configure();
            var type = typeof(ModuleEngines).Assembly.GetType("Expansions.BundleLoader+<LoadAssetBundle>d__16", true);
            var target = AccessTools.DeclaredMethod(type, "MoveNext");
            var patcher = new Harmony(Id);
            try
            {
                patcher.Patch(target, null, null, new HarmonyMethod(typeof(Patch), "Transpile") { priority = Priority.Last });
            }
            catch
            {
                patcher.Unpatch(target, HarmonyPatchType.All, Id);
                throw;
            }

            harmony = patcher;
            UnityEngine.Debug.Log("[DlcCacheFix] Loaded 1.0.0: original integrity hash retained; verified LZ4 cache or original-buffer fallback.");
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            int hashes = 0, loads = 0, first = -1, last = -1;
            for (int i = 0; i < code.Count; i++)
            {
                if (code[i].Calls(Hash))
                {
                    code[i].opcode = OpCodes.Call;
                    code[i].operand = typeof(Patch).GetMethod("ComputeHash");
                    hashes++;
                    first = i;
                }

                if (code[i].Calls(Load))
                {
                    code[i].opcode = OpCodes.Call;
                    code[i].operand = typeof(Patch).GetMethod("LoadBundle");
                    loads++;
                    last = i;
                }
            }

            if (hashes != 1 || loads != 1 || first >= last)
                throw new NotSupportedException("The DLC loader no longer has the expected hash and bundle-load calls.");
            for (int i = first + 1; i <= last; i++)
                if (code[i].labels.Count != 0
                    || code[i].blocks.Count != 0
                    || code[i].opcode.FlowControl == FlowControl.Branch
                    || code[i].opcode.FlowControl == FlowControl.Cond_Branch
                    || code[i].opcode.FlowControl == FlowControl.Return)
                    throw new NotSupportedException("DLC hash/load path can yield or branch.");
            return code;
        }

        public static byte[] ComputeHash(HashAlgorithm algorithm, byte[] data)
        {
            digest = null;
            long begin = Stopwatch.GetTimestamp();
            try
            {
                digest = algorithm.ComputeHash(data);
                return digest;
            }
            finally
            {
                HashTicks += Stopwatch.GetTimestamp() - begin;
            }
        }

        public static AssetBundle LoadBundle(byte[] original)
        {
            byte[] hash = digest;
            digest = null;
            if (Preparation != null
                && Preparation.IsCompleted
                && !Preparation.IsFaulted
                && Store != null
                && hash != null
                && hash.Length == 16)
            {
                string path;
                long begin = Stopwatch.GetTimestamp();
                bool valid = Store.TryResolve(CacheStore.Hex(hash), out path);
                ValidationTicks += Stopwatch.GetTimestamp() - begin;
                if (valid)
                {
                    begin = Stopwatch.GetTimestamp();
                    try
                    {
                        AssetBundle cached = AssetBundle.LoadFromFile(path);
                        if (cached != null)
                        {
                            CacheHits++;
                            return cached;
                        }
                    }
                    catch (UnityException ex)
                    {
                        UnityEngine.Debug.LogWarning("[DlcCacheFix] Cache rejected: " + ex.Message);
                    }
                    finally
                    {
                        CacheLoadTicks += Stopwatch.GetTimestamp() - begin;
                    }

                    RejectedCaches++;
                }
            }

            long start = Stopwatch.GetTimestamp();
            try
            {
                RawLoads++;
                return AssetBundle.LoadFromMemory(original);
            }
            finally
            {
                RawLoadTicks += Stopwatch.GetTimestamp() - start;
            }
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
                UnityEngine.Debug.LogError("[DlcCacheFix] Disabled: " + ex);
            }

            Destroy(this);
        }
    }
}
