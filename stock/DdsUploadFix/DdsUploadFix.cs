using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering DDS Upload Fix")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace DdsUploadFix
{
    public struct PinnedPayload : IDisposable
    {
        GCHandle handle;
        public readonly IntPtr Pointer;
        public readonly int Count;
        public PinnedPayload(BinaryReader reader, int count)
        {
            if (count < 0) throw new ArgumentOutOfRangeException("count");
            var stream = reader.BaseStream as MemoryStream;
            if (stream == null) throw new NotSupportedException("Expected DDS memory stream.");
            long position = stream.Position;
            Count = (int)Math.Min(count, Math.Max(0, stream.Length - position));
            byte[] bytes = stream.GetBuffer();
            handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            Pointer = IntPtr.Add(handle.AddrOfPinnedObject(), checked((int)Math.Min(position, bytes.Length)));
            stream.Position = position + Count;
        }
        public void Dispose() { if (handle.IsAllocated) handle.Free(); }
    }

    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.dds-upload";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static readonly ConstructorInfo Memory = typeof(MemoryStream).GetConstructor(new[] { typeof(byte[]) });
        static readonly MethodInfo ReadBytes = typeof(BinaryReader).GetMethod("ReadBytes", new[] { typeof(int) });
        static readonly MethodInfo LoadBytes = typeof(Texture2D).GetMethod("LoadRawTextureData", new[] { typeof(byte[]) });
        static Harmony harmony;
        public static long AvoidedCopies, AvoidedCopyBytes;

        public static MemoryStream OpenBuffer(byte[] bytes)
        { return new MemoryStream(bytes, 0, bytes.Length, false, true); }

        public static void Upload(Texture2D texture, BinaryReader reader, int count)
        {
            using (var payload = new PinnedPayload(reader, count))
            {
                texture.LoadRawTextureData(payload.Pointer, payload.Count);
                AvoidedCopies++; AvoidedCopyBytes += payload.Count;
            }
        }

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.DdsDirectUpload") && !KerbalOverEngineering.Options.Settings.Enabled("Stock.DdsPrefetch")) return;
            if (harmony != null) return;
            if (typeof(DatabaseLoaderTexture_DDS).Assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("Unsupported KSP DDS loader.");
            var target = AccessTools.DeclaredMethod(typeof(DatabaseLoaderTexture_DDS), "Read");
            var patcher = new Harmony(Id);
            try
            {
                patcher.Patch(target, null, null, new HarmonyMethod(typeof(Patch), "Transpile"));
                if (KerbalOverEngineering.Options.Settings.Enabled("Stock.DdsPrefetch")) PrefetchPhase.Register();
            }
            catch { patcher.Unpatch(target, HarmonyPatchType.All, Id); throw; }
            harmony = patcher;
            Debug.Log("[DdsUploadFix] Loaded 1.0.0: upload original DDS payload without a second managed copy; DXT data and mipmaps unchanged.");
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            bool direct = KerbalOverEngineering.Options.Settings.Enabled("Stock.DdsDirectUpload");
            bool prefetch = KerbalOverEngineering.Options.Settings.Enabled("Stock.DdsPrefetch");
            int streams = 0, uploads = 0, reads = 0, files = 0;
            for (int i = 0; i < code.Count; i++)
            {
                if (code[i].Calls(typeof(File).GetMethod("ReadAllBytes", new[] { typeof(string) })))
                { if (prefetch) code[i].operand = typeof(PrefetchPhase).GetMethod("Read"); files++; }
                if (!direct) continue;
                if (code[i].opcode == OpCodes.Newobj && Equals(code[i].operand, Memory))
                {
                    code[i].opcode = OpCodes.Call; code[i].operand = typeof(Patch).GetMethod("OpenBuffer"); streams++;
                }
                if (!code[i].Calls(ReadBytes)) continue;
                reads++;
                int next = i + 1;
                if (next + 1 < code.Count && code[next].opcode == OpCodes.Dup && code[next + 1].opcode == OpCodes.Pop) next += 2;
                if (next >= code.Count || !code[next].Calls(LoadBytes)) throw new NotSupportedException("DDS payload consumer changed.");
                for (int j = i; j <= next; j++)
                    if (code[j].blocks.Count != 0 || code[j].labels.Count != 0)
                        throw new NotSupportedException("DDS payload control flow changed.");
                // Keep texture, reader and count on the stack until the combined upload call.
                code[i].opcode = OpCodes.Nop; code[i].operand = null;
                for (int j = i + 1; j < next; j++) { code[j].opcode = OpCodes.Nop; code[j].operand = null; }
                code[next].opcode = OpCodes.Call; code[next].operand = typeof(Patch).GetMethod("Upload"); uploads++;
            }
            if (files != 1 || (direct && (streams != 1 || reads != 2 || uploads != 2))) throw new NotSupportedException("Expected one DDS buffer and DXT1/DXT5 uploads.");
            return code;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try { Patch.Apply(); }
            catch (Exception ex) { Debug.LogError("[DdsUploadFix] Disabled: " + ex); }
            Destroy(this);
        }
    }
}
