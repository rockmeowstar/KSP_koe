using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Expansion Buffer Fix")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace ExpansionBufferFix
{
    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.expansion-buffer";
        const string KspSha = "d9e42483f25ee80a9c11d6c1c0a0d29b4ec78c1e08d76c971b71580c9cce51e4";
        static readonly Guid Supported = new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba");
        static readonly MethodInfo Hash = typeof(HashAlgorithm).GetMethod("ComputeHash", new[] { typeof(byte[]) });
        static Harmony harmony;
        public static long AvoidedCopies, AvoidedCopyBytes;

        public static MethodInfo FindTarget(Assembly assembly)
        {
            Type type = assembly.GetType("Expansions.BundleLoader+<LoadAssetBundle>d__16", true);
            var method = AccessTools.DeclaredMethod(type, "MoveNext");
            if (method == null || method.ReturnType != typeof(bool)) throw new NotSupportedException("Expansion loader layout changed.");
            return method;
        }

        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.ExpansionBufferReuse")) return;
            if (harmony != null) return;
            Assembly assembly = typeof(ModuleEngines).Assembly;
            if (assembly.ManifestModule.ModuleVersionId != Supported) throw new NotSupportedException("Unsupported KSP assembly.");
            if (!string.IsNullOrEmpty(assembly.Location)) using (var sha = SHA256.Create())
                if (BitConverter.ToString(sha.ComputeHash(System.IO.File.ReadAllBytes(assembly.Location))).Replace("-", "").ToLowerInvariant() != KspSha)
                    throw new NotSupportedException("KSP file identity changed.");
            var target = FindTarget(assembly);
            var patcher = new Harmony(Id);
            try { patcher.Patch(target, null, null, new HarmonyMethod(typeof(Patch), "Transpile")); }
            catch { patcher.Unpatch(target, HarmonyPatchType.All, Id); throw; }
            harmony = patcher;
            Debug.Log("[ExpansionBufferFix] Loaded 1.0.0: one downloaded buffer for MD5 and bundle load; integrity checks preserved.");
        }

        static bool IsDataRead(CodeInstruction instruction)
        {
            var method = instruction.operand as MethodInfo;
            return instruction.opcode == OpCodes.Callvirt && method != null &&
                method.DeclaringType.FullName == "UnityEngine.Networking.DownloadHandler" && method.Name == "get_data" &&
                method.ReturnType == typeof(byte[]) && method.GetParameters().Length == 0;
        }

        static FieldInfo RequestField(List<CodeInstruction> code, int data)
        {
            int i = data - 1;
            if (i > 0 && code[i].opcode == OpCodes.Pop && code[i - 1].opcode == OpCodes.Dup) i -= 2;
            var getter = i >= 2 ? code[i].operand as MethodInfo : null;
            var field = i >= 2 ? code[i - 1].operand as FieldInfo : null;
            if (getter == null || code[i].opcode != OpCodes.Callvirt || getter.Name != "get_downloadHandler" ||
                getter.DeclaringType.FullName != "UnityEngine.Networking.UnityWebRequest" ||
                code[i - 1].opcode != OpCodes.Ldfld || field == null ||
                field.FieldType.FullName != "UnityEngine.Networking.UnityWebRequest" || code[i - 2].opcode != OpCodes.Ldarg_0)
                throw new NotSupportedException("Unexpected expansion download handler.");
            return field;
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source, ILGenerator generator)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            var reads = Enumerable.Range(0, code.Count).Where(i => IsDataRead(code[i])).ToArray();
            if (reads.Length != 2 || !Equals(code[reads[0]].operand, code[reads[1]].operand) ||
                !Equals(RequestField(code, reads[0]), RequestField(code, reads[1])))
                throw new NotSupportedException("Expected two reads of the same expansion buffer.");
            int first = reads[0], second = reads[1], hashes = 0, handlers = 0;
            for (int i = first; i <= second; i++)
            {
                var c = code[i];
                if (c.labels.Count != 0 || c.blocks.Count != 0 ||
                    c.opcode.FlowControl == FlowControl.Branch || c.opcode.FlowControl == FlowControl.Cond_Branch ||
                    c.opcode.FlowControl == FlowControl.Return || c.opcode.FlowControl == FlowControl.Throw)
                    throw new NotSupportedException("Expansion buffer control flow changed.");
                var method = c.operand as MethodInfo;
                if (method == null || i == first || i == second) continue;
                if (Equals(method, Hash)) hashes++;
                else if (method.DeclaringType.FullName == "UnityEngine.Networking.UnityWebRequest" && method.Name == "get_downloadHandler") handlers++;
                else throw new NotSupportedException("Unexpected operation between expansion buffer reads.");
            }
            int next = second + 1;
            if (next + 1 < code.Count && code[next].opcode == OpCodes.Dup && code[next + 1].opcode == OpCodes.Pop) next += 2;
            var load = next < code.Count ? code[next].operand as MethodInfo : null;
            if (hashes != 1 || handlers != 1 || load == null || load.DeclaringType.FullName != "UnityEngine.AssetBundle" ||
                load.Name != "LoadFromMemory" || !load.IsStatic || load.GetParameters().Length != 1 || load.GetParameters()[0].ParameterType != typeof(byte[]))
                throw new NotSupportedException("Expansion hash/load sequence changed.");

            LocalBuilder buffer = generator.DeclareLocal(typeof(byte[]));
            var result = new List<CodeInstruction>(code.Count + 4);
            for (int i = 0; i < code.Count; i++)
            {
                if (i == second)
                {
                    var pop = new CodeInstruction(code[i]); pop.opcode = OpCodes.Pop; pop.operand = null;
                    result.Add(pop); // Discard the handler, retaining its original evaluation order.
                    result.Add(new CodeInstruction(OpCodes.Ldloc, buffer));
                    result.Add(new CodeInstruction(OpCodes.Call, typeof(Patch).GetMethod("RecordReuse")));
                }
                else result.Add(code[i]);
                if (i == first)
                {
                    result.Add(new CodeInstruction(OpCodes.Dup));
                    result.Add(new CodeInstruction(OpCodes.Stloc, buffer));
                }
            }
            return result;
        }

        public static byte[] RecordReuse(byte[] buffer)
        {
            if (buffer != null) { AvoidedCopies++; AvoidedCopyBytes += buffer.LongLength; }
            return buffer;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try { Patch.Apply(); }
            catch (Exception ex) { Debug.LogError("[ExpansionBufferFix] Disabled: " + ex.Message); }
            Destroy(this);
        }
    }
}
