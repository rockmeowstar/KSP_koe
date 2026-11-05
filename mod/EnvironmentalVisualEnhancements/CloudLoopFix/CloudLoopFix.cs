using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using HarmonyLib;
using UnityEngine;

[assembly: AssemblyTitle("KerbalOverEngineering Cloud Loop Fix")]
[assembly: AssemblyVersion("1.2.4.0")]
namespace CloudLoopFix
{
    public static class Patch
    {
        const string Id = "local.kerbaloverengineering.cloud-loops";
        public const string AtmosphereSha = "fab4af80399bddc035d7d048fa6fa0d2685de3e6a0b2f275f7800e126d2f6d41";
        static readonly Guid Supported = new Guid("8150e218-893c-4b2c-a852-b674a3006cfd");
        static Harmony harmony;
        static MethodInfo intervals, commands;
        public static void Apply()
        {
            bool geometry = KerbalOverEngineering.Options.Settings.Enabled("EVE.IntervalGeometryMemo");
            bool order = KerbalOverEngineering.Options.Settings.Enabled("EVE.LayerOrderMemo");
            bool material = KerbalOverEngineering.Options.Settings.Enabled("EVE.MaterialSetupDedup");
            if (!geometry && !order && !material)
                return;
            if (harmony != null)
                return;
            Assembly assembly = AppDomain.CurrentDomain.GetAssemblies().SingleOrDefault(a => a.GetName().Name == "Atmosphere");
            if (assembly == null)
                return;
            if (assembly.ManifestModule.ModuleVersionId != Supported)
                throw new NotSupportedException("Unsupported Atmosphere assembly.");
            // AI-generated comment: Mono/KSP may load an assembly from bytes (empty Location); the installer checks the full file hash.
            if (!string.IsNullOrEmpty(assembly.Location))
                using (var sha = SHA256.Create())
                    if (BitConverter.ToString(sha.ComputeHash(System.IO.File.ReadAllBytes(assembly.Location))).Replace("-", "").ToLowerInvariant() != AtmosphereSha)
                        throw new NotSupportedException("Atmosphere file identity changed.");
            Prepare(assembly);
            var patcher = new Harmony(Id);
            try
            {
                Type[] args =
                {
                    intervals.GetParameters()[0].ParameterType.GetGenericArguments()[0],
                    intervals.ReturnType.GetGenericArguments()[0]
                };
                Type hooks = typeof(IntervalHooks<, >).MakeGenericType(args);
                if (geometry)
                    patcher.Patch(intervals, new HarmonyMethod(hooks.GetMethod("Prefix")), new HarmonyMethod(hooks.GetMethod("Postfix")));
                if (order)
                    patcher.Patch(commands, null, null, new HarmonyMethod(typeof(Patch), "Transpile"));
                if (material)
                    patcher.Patch(
                        commands,
                        new HarmonyMethod(typeof(MaterialSetupScope), "Begin"),
                        null,
                        new HarmonyMethod(typeof(Patch), "TranspileMaterialSetup"),
                        new HarmonyMethod(typeof(MaterialSetupScope), "End")
                    );
            }
            catch
            {
                patcher.Unpatch(intervals, HarmonyPatchType.All, Id);
                patcher.Unpatch(commands, HarmonyPatchType.All, Id);
                throw;
            }

            harmony = patcher;
            Debug.Log(
                "[CloudLoopFix] Loaded 1.2.4: exact layer interval/order memoization and invocation-local material setup; GPU visibility culling reports separately."
            );
        }

        public static void Prepare(Assembly assembly)
        {
            Type renderer = assembly.GetType("Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer", true);
            intervals = AccessTools.DeclaredMethod(renderer, "ResolveLayerOverlapIntervals");
            commands = AccessTools.DeclaredMethod(renderer, "HandleRenderingCommands");
            if (intervals == null || commands == null)
                throw new NotSupportedException("Cannot find EVE's ResolveLayerOverlapIntervals or HandleRenderingCommands method.");
            // AI-generated comment: Validate both sites before installing either patch.
            new List<CodeInstruction>(Transpile(PatchProcessor.GetOriginalInstructions(commands)));
            var validation = new DynamicMethod("KOE_ValidateMaterialSetup", typeof(void), Type.EmptyTypes);
            new List<CodeInstruction>(TranspileMaterialSetup(PatchProcessor.GetOriginalInstructions(commands), validation.GetILGenerator()));
        }

        public static IEnumerable<CodeInstruction> TranspileMaterialSetup(IEnumerable<CodeInstruction> source, ILGenerator generator)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            string[] properties =
            {
                "reconstructedTextureResolution_PROPERTY",
                "invReconstructedTextureResolution_PROPERTY",
                "paddedReconstructedTextureResolution_PROPERTY",
                "reprojectionXfactor_PROPERTY",
                "reprojectionYfactor_PROPERTY",
                "CameraToWorld_PROPERTY",
                "reprojectionUVOffset_PROPERTY",
                "outerLayerRadius_PROPERTY"
            };
            int first = -1, last = -1;
            for (int i = 0; i < code.Count; i++)
            {
                var getter = code[i].operand as MethodInfo;
                if (getter == null || getter.DeclaringType.FullName != "Utils.ShaderProperties")
                    continue;
                if (getter.Name == "get_" + properties[0])
                {
                    if (first >= 0)
                        throw new NotSupportedException("Ambiguous cloud material setup.");
                    first = i - 1;
                }

                if (getter.Name == "get_" + properties[properties.Length - 1])
                    last = i;
            }

            if (first < 0 || last <= first || !IsLoadLocal(code[first]))
                throw new NotSupportedException("EVE's material setup no longer has the expected property setters.");
            while (last < code.Count && !(code[last].operand is MethodInfo && ((MethodInfo)code[last].operand).Name == "SetFloat"))
                last++;
            if (last + 1 >= code.Count)
                throw new NotSupportedException("Cloud material setup end missing.");
            int writes = 0, property = 0;
            for (int i = first; i <= last; i++)
            {
                if (code[i].blocks.Count != 0
                    || (i > first
                    && code[i].labels.Count != 0)
                    || code[i].opcode.FlowControl == FlowControl.Branch
                    || code[i].opcode.FlowControl == FlowControl.Cond_Branch)
                    throw new NotSupportedException("Cloud material setup control flow changed.");
                var method = code[i].operand as MethodInfo;
                if (method != null && method.DeclaringType.FullName == "Utils.ShaderProperties")
                {
                    if (property >= properties.Length || method.Name != "get_" + properties[property++])
                        throw new NotSupportedException("Cloud material setup properties changed.");
                }

                if (method != null && method.DeclaringType == typeof(Material) && method.Name.StartsWith("Set"))
                    writes++;
            }

            if (property != properties.Length || writes != 8)
                throw new NotSupportedException("Expected exactly eight invariant material writes.");
            Label skip = generator.DefineLabel();
            code[last + 1].labels.Add(skip);
            var load = new CodeInstruction(code[first]);
            code[first].labels.Clear();
            code.InsertRange(
                first,
                new[] {
                load,
                new CodeInstruction(OpCodes.Call, typeof(MaterialSetupScope).GetMethod("ShouldSetup")),
                new CodeInstruction(OpCodes.Brfalse, skip)
            }
            );
            return code;
        }

        static bool IsLoadLocal(CodeInstruction code)
        {
            return code.opcode == OpCodes.Ldloc
                || code.opcode == OpCodes.Ldloc_S
                || code.opcode == OpCodes.Ldloc_0
                || code.opcode == OpCodes.Ldloc_1
                || code.opcode == OpCodes.Ldloc_2
                || code.opcode == OpCodes.Ldloc_3;
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>(source);
            int replacements = 0;
            for (int i = 0; i + 1 < code.Count; i++)
            {
                var order = code[i].operand as MethodInfo;
                var toList = code[i + 1].operand as MethodInfo;
                if (order == null
                    || toList == null
                    || order.DeclaringType != typeof(Enumerable)
                    || order.Name != "OrderBy"
                    || toList.DeclaringType != typeof(Enumerable)
                    || toList.Name != "ToList")
                    continue;
                Type[] args = order.GetGenericArguments();
                if (args.Length != 2
                    || args[1] != typeof(float)
                    || args[0].FullName != "Atmosphere.CloudsRaymarchedVolume"
                    || order.GetParameters().Length != 2
                    || toList.GetGenericArguments()[0] != args[0]
                    || code[i + 1].labels.Count != 0
                    || code[i + 1].blocks.Count != 0)
                    throw new NotSupportedException("Cloud sort IL changed.");
                var replacement = new CodeInstruction(code[i]);
                replacement.opcode = OpCodes.Call;
                replacement.operand = typeof(LayerOrder<>).MakeGenericType(args[0]).GetMethod("Sort");
                code[i] = replacement;
                code[i + 1] = new CodeInstruction(OpCodes.Nop);
                replacements++;
            }

            if (replacements != 1)
                throw new NotSupportedException("Expected exactly one cloud layer sort.");
            return code;
        }

        public static Func<T, BoundKey> MakeBoundReader<T>()
        {
            Type type = typeof(T);
            var radius = AccessTools.DeclaredField(type, "radius");
            var layer = AccessTools.DeclaredField(type, "layer");
            var nature = AccessTools.DeclaredField(type, "nature");
            if (!type.IsValueType
                || radius == null
                || radius.FieldType != typeof(float)
                || layer == null
                || layer.FieldType.IsValueType
                || nature == null
                || !nature.FieldType.IsEnum
                || Enum.GetUnderlyingType(nature.FieldType) != typeof(int))
                throw new NotSupportedException("Unsupported cloud layer bound layout.");
            var method = new DynamicMethod("KOE_BoundKey", typeof(BoundKey), new[] { type }, typeof(Patch), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarga_S, (byte)0);
            il.Emit(OpCodes.Ldfld, radius);
            il.Emit(OpCodes.Ldarga_S, (byte)0);
            il.Emit(OpCodes.Ldfld, layer);
            il.Emit(OpCodes.Ldarga_S, (byte)0);
            il.Emit(OpCodes.Ldfld, nature);
            il.Emit(OpCodes.Newobj, typeof(BoundKey).GetConstructor(new[] { typeof(float), typeof(object), typeof(int) }));
            il.Emit(OpCodes.Ret);
            return (Func<T, BoundKey>)method.CreateDelegate(typeof(Func<T, BoundKey>));
        }
    }

    public static class IntervalHooks<TBound, TInterval>
    {
        static readonly Func<TBound, BoundKey> Key = Patch.MakeBoundReader<TBound>();
        static readonly ConditionalWeakTable<object, ListMemo<TBound, BoundKey, List<TInterval>>> Entries = new ConditionalWeakTable<object, ListMemo<TBound, BoundKey, List<TInterval>>>();
        static readonly ConditionalWeakTable<object, ListMemo<TBound, BoundKey, List<TInterval>>>.CreateValueCallback Create = delegate
        {
            return new ListMemo<TBound, BoundKey, List<TInterval>>();
        };
        public static bool Prefix(object __instance, List<TBound> __0, ref List<TInterval> __result, out bool __state)
        {
            __state = Entries.GetValue(__instance, Create).TryGet(__0, Key, out __result);
            return !__state;
        }

        public static void Postfix(object __instance, List<TBound> __0, List<TInterval> __result, bool __state)
        {
            if (!__state)
                Entries.GetValue(__instance, Create).Commit(__0, Key, __result);
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
                Debug.LogError("[CloudLoopFix] Disabled: " + ex.Message);
            }

            Destroy(this);
        }
    }
}
