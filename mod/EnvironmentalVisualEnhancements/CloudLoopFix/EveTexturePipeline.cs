using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using HarmonyLib;
using UnityEngine;

namespace CloudLoopFix
{
    public struct EvePayload : IDisposable
    {
        GCHandle pin;
        public readonly IntPtr Pointer;
        public readonly int Count;
        public EvePayload(BinaryReader reader, int count)
        {
            if (count < 0)
                throw new ArgumentOutOfRangeException("count");
            var stream = reader.BaseStream as MemoryStream;
            if (stream == null)
                throw new NotSupportedException("Expected EVE DDS buffer");
            byte[] bytes = stream.GetBuffer();
            long position = stream.Position;
            Count = (int)Math.Min(count, Math.Max(0, stream.Length - position));
            pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            Pointer = IntPtr.Add(pin.AddrOfPinnedObject(), checked((int)Math.Min(position, bytes.Length)));
            stream.Position = position + Count;
        }

        public void Dispose()
        {
            if (pin.IsAllocated)
                pin.Free();
        }
    }

    public static class EveTexturePatch
    {
        static EarlyFileBank bank;
        static bool closed;
        public static long AvoidedCopies, AvoidedCopyBytes, PrefetchedFiles, PrefetchedBytes;
        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("EVE.DdsDirectUpload")
                && !KerbalOverEngineering.Options.Settings.Enabled("EVE.TexturePrefetch"))
                return;
            const string id = "local.kerbaloverengineering.eve-texture-pipeline";
            var assembly = AppDomain.CurrentDomain.GetAssemblies().Single(a => a.GetName().Name == "Utils");
            if (assembly.ManifestModule.ModuleVersionId != new Guid("752c32c2-9c32-40be-bad9-8a27f80a0a26"))
                throw new NotSupportedException("The EVE Utils assembly does not match the supported texture-loader build.");
            var target = AccessTools.DeclaredMethod(assembly.GetType("Utils.TextureConverter", true), "DDSToTexture");
            var harmony = new Harmony(id);
            try
            {
                harmony.Patch(target, null, null, new HarmonyMethod(typeof(EveTexturePatch), "Transpile"));
                if (KerbalOverEngineering.Options.Settings.Enabled("EVE.TexturePrefetch"))
                    harmony.Patch(AccessTools.DeclaredMethod(typeof(PartLoader), "StartLoad"), new HarmonyMethod(typeof(EveTexturePatch), "Prepare"));
            }
            catch
            {
                harmony.UnpatchAll(id);
                throw;
            }
        }

        public static MemoryStream Open(byte[] bytes)
        {
            return new MemoryStream(bytes, 0, bytes.Length, false, true);
        }

        public static void Upload(Texture2D texture, BinaryReader reader, int count)
        {
            using (var payload = new EvePayload(reader, count))
            {
                texture.LoadRawTextureData(payload.Pointer, payload.Count);
                AvoidedCopies++;
                AvoidedCopyBytes += payload.Count;
            }
        }

        public static byte[] Read(string path)
        {
            byte[] data;
            if (bank != null && bank.TryTake(path, out data))
            {
                PrefetchedFiles++;
                PrefetchedBytes += data.LongLength;
                return data;
            }

            return File.ReadAllBytes(path);
        }

        static void Collect(ConfigNode node, HashSet<string> paths, string root)
        {
            foreach (ConfigNode.Value value in node.values)
            {
                string text = value.value;
                if (string.IsNullOrEmpty(text) || text.IndexOf('/') < 0 || text.Contains("..") || Path.IsPathRooted(text))
                    continue;
                string path = Path.GetFullPath(Path.Combine(root, text + ".dds"));
                if (path.StartsWith(root, StringComparison.OrdinalIgnoreCase) && File.Exists(path))
                    paths.Add(path);
            }

            foreach (ConfigNode child in node.nodes)
                Collect(child, paths, root);
        }

        static void Prepare()
        {
            if (bank != null || closed)
                return;
            try
            {
                var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                string root = Path.GetFullPath(Path.Combine(KSPUtil.ApplicationRootPath, "GameData")) + Path.DirectorySeparatorChar;
                // AI-generated comment: ModuleManager has finished before PartLoader. Snapshot config values on the main thread.
                foreach (var config in GameDatabase.Instance.GetConfigs("EVE_CLOUDS"))
                    foreach (var node in config.config.GetNodes("OBJECT"))
                        Collect(node, paths, root);
                var resources = KerbalOverEngineering.Options.LoadingResources.Current;
                if (resources.EveBudget <= 0)
                    return;
                bank = new EarlyFileBank(paths, resources.EveBudget, resources.Workers);
                Debug.Log("[CloudLoopFix] EVE file preparation during PartLoader: files=" + bank.SelectedFiles
                    + "; budgetBytes=" + bank.ReservedBytes);
            }
            catch (Exception ex)
            {
                Debug.LogWarning("[CloudLoopFix] EVE preparation skipped: " + ex.Message);
            }
        }

        public static void Release()
        {
            closed = true;
            var old = bank;
            bank = null;
            if (old != null)
                old.Dispose();
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> input)
        {
            var code = input.Select(c => new CodeInstruction(c)).ToList();
            int files = 0, streams = 0, reads = 0, uploads = 0;
            bool direct = KerbalOverEngineering.Options.Settings.Enabled("EVE.DdsDirectUpload");
            bool prefetch = KerbalOverEngineering.Options.Settings.Enabled("EVE.TexturePrefetch");
            var memory = typeof(MemoryStream).GetConstructor(new[] { typeof(byte[]) });
            var read = typeof(BinaryReader).GetMethod("ReadBytes", new[] { typeof(int) });
            var load = typeof(Texture2D).GetMethod("LoadRawTextureData", new[] { typeof(byte[]) });
            foreach (var c in code)
            {
                if (c.Calls(typeof(File).GetMethod("ReadAllBytes", new[] { typeof(string) })))
                {
                    if (prefetch)
                        c.operand = AccessTools.Method(typeof(EveTexturePatch), "Read");
                    files++;
                }

                if (direct && c.opcode == OpCodes.Newobj && Equals(c.operand, memory))
                {
                    c.opcode = OpCodes.Call;
                    c.operand = AccessTools.Method(typeof(EveTexturePatch), "Open");
                    streams++;
                }
            }

            for (int i = 0; direct && i < code.Count; i++)
                if (code[i].Calls(read))
                {
                    reads++;
                    int j = i + 1;
                    if (j + 1 < code.Count && code[j].opcode == OpCodes.Dup && code[j + 1].opcode == OpCodes.Pop)
                        j += 2;
                    if (j >= code.Count || !code[j].Calls(load))
                        throw new NotSupportedException("EVE DDS consumer changed");
                    for (int k = i; k <= j; k++)
                        if (code[k].labels.Count != 0 || code[k].blocks.Count != 0)
                            throw new NotSupportedException("EVE DDS control flow changed");
                    for (int k = i; k < j; k++)
                    {
                        code[k].opcode = OpCodes.Nop;
                        code[k].operand = null;
                    }

                    code[j].opcode = OpCodes.Call;
                    code[j].operand = AccessTools.Method(typeof(EveTexturePatch), "Upload");
                    uploads++;
                }

            if (files != 1 || (direct && (streams != 1 || reads != 14 || uploads != 14)))
                throw new NotSupportedException("Cannot find the texture creation and upload calls expected in the EVE DDS loader.");
            return code;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class EveTextureBootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try
            {
                EveTexturePatch.Apply();
                DontDestroyOnLoad(gameObject);
                GameEvents.onLevelWasLoadedGUIReady.Add(Ready);
            }
            catch (Exception ex)
            {
                Debug.LogError("[CloudLoopFix] EVE texture pipeline disabled: " + ex);
                Destroy(this);
            }
        }

        void Ready(GameScenes scene)
        {
            if (scene != GameScenes.MAINMENU && scene != GameScenes.LOADING)
            {
                EveTexturePatch.Release();
                Destroy(this);
            }
        }

        public void OnDestroy()
        {
            GameEvents.onLevelWasLoadedGUIReady.Remove(Ready);
            EveTexturePatch.Release();
        }
    }
}
