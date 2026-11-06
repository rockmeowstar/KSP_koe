using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Threading;
using HarmonyLib;
using UnityEngine;

namespace EveCurveBakeFix
{
    public static class EveCurveKernel
    {
        const string Id = "local.kerbaloverengineering.eve-curve-bake";
        static MethodInfo target, clone;
        static FieldInfo resolution, input, output;
        static int mode, rejected;
        static bool active;
        static int compatible = 1;
        static MethodInfo[] critical;
        static Action<string, string> emit;
        static readonly ConcurrentBag<Color[]> buffers = new ConcurrentBag<Color[]>();
        public static long Columns, Checks, Mismatches, Fallback, RemovedLerps, OriginalTicks, FastTicks;
        public struct ColumnState
        {
            public long Start;
            public Color[] Oracle, Output;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void OriginalColumn(object instance, int x)
        {
            throw new NotImplementedException("Reverse patch not installed");
        }

        public static MethodInfo Target
        {
            get
            {
                return target;
            }
        }

        public static void Apply(Assembly assembly, int selectedMode, Action<string, string> report, bool profileBake = true)
        {
            if (active)
                throw new InvalidOperationException("Curve experiment already initialized");
            if (selectedMode < 1 || selectedMode > 3)
                throw new ArgumentOutOfRangeException("selectedMode");
            var volume = assembly.GetType("Atmosphere.CloudsRaymarchedVolume", true);
            var closure = volume.GetNestedTypes(BindingFlags.NonPublic | BindingFlags.Public)
                .Single(t => AccessTools.DeclaredMethod(t, "<BakeCurvesTexture>b__0") != null);
            target = AccessTools.DeclaredMethod(closure, "<BakeCurvesTexture>b__0");
            KerbalOverEngineering.Options.ManagedContract.Require(target, "6c97e7cb1c80114e55a6c3fc6ea301a12964d6172872e17c8cfd0c758bf9c7f8");
            var body = target.GetMethodBody();
            if (body.LocalVariables.Count != 9
                || body.LocalVariables[8].LocalType != typeof(float)
                || body.ExceptionHandlingClauses.Count != 0)
                throw new NotSupportedException("Cannot find the interpolation calls expected in EVE's curve-column baker.");
            var methods = new List<MethodInfo>();
            foreach (var getter in new[]
            {
                assembly.GetType("Atmosphere.CloudType", true).GetProperty("NoiseEdgeHardness").GetGetMethod(),
                assembly.GetType("Atmosphere.CloudType", true).GetProperty("Density").GetGetMethod()
            }

            )
            {
                var bytes = getter.GetMethodBody().GetILAsByteArray();
                if (bytes.Length != 7 || bytes[0] != 2 || bytes[1] != 0x7b || bytes[6] != 0x2a || Harmony.GetPatchInfo(getter) != null)
                    throw new NotSupportedException("Curve getter is not unmodified field access");
                methods.Add(getter);
            }

            foreach (var name in new[]
            {
                "Lerp",
                "Clamp01",
                "Max"
            }

            )
            {
                var args = name == "Lerp" ? new[]
                {
                    typeof(float),
                    typeof(float),
                    typeof(float)
                }

                : name == "Max" ? new[]
                {
                    typeof(float),
                    typeof(float)
                }

                : new[]
                {
                    typeof(float)
                };
                var method = typeof(Mathf).GetMethod(name, args);
                if (Harmony.GetPatchInfo(method) != null)
                    throw new NotSupportedException("Mathf is already patched");
                methods.Add(method);
            }

            critical = methods.ToArray();
            if (Harmony.GetPatchInfo(target) != null)
                throw new NotSupportedException("Curve column is already patched");
            resolution = AccessTools.Field(closure, "resolution");
            input = AccessTools.Field(closure, "curvesColors");
            output = AccessTools.Field(closure, "accumulatedVerticalCoverageColors");
            if (resolution == null
                || resolution.FieldType != typeof(int)
                || input == null
                || input.FieldType != typeof(Color[])
                || output == null
                || output.FieldType != typeof(Color[]))
                throw new NotSupportedException("EVE's curve baker must expose a resolution and two Color[] fields.");
            clone = typeof(object).GetMethod("MemberwiseClone", BindingFlags.Instance | BindingFlags.NonPublic);
            mode = selectedMode;
            emit = report;
            var harmony = new Harmony(Id);
            try
            {
                harmony.CreateReversePatcher(target, new HarmonyMethod(typeof(EveCurveKernel), "OriginalColumn")).Patch(HarmonyReversePatchType.Original);
                harmony.Patch(
                    target,
                    new HarmonyMethod(typeof(EveCurveKernel), "Begin"),
                    new HarmonyMethod(typeof(EveCurveKernel), "End"),
                    selectedMode == 1 ? null : new HarmonyMethod(typeof(EveCurveKernel), "Transpile")
                );
                if (profileBake)
                    harmony.Patch(
                        AccessTools.DeclaredMethod(volume, "BakeCurvesTexture"),
                        new HarmonyMethod(typeof(EveCurveKernel), "BakeBegin"),
                        null,
                        null,
                        new HarmonyMethod(typeof(EveCurveKernel), "BakeEnd")
                    );
                active = true;
            }
            catch
            {
                harmony.UnpatchAll(Id);
                throw;
            }
        }

        public static bool Begin(object __instance, int x, out ColumnState __state)
        {
            __state = new ColumnState();
            var colors = (Color[])input.GetValue(__instance);
            var result = (Color[])output.GetValue(__instance);
            if (Interlocked.CompareExchange(ref rejected, 0, 0) != 0
                || Interlocked.CompareExchange(ref compatible, 0, 0) == 0
                || (int)resolution.GetValue(__instance) != 128
                || x < 0
                || x >= 128
                || colors == null
                || colors.Length < 16384
                || result == null
                || result.Length < 16384)
            {
                Interlocked.Increment(ref Fallback);
                OriginalColumn(__instance, x);
                return false;
            }

            Interlocked.Increment(ref Columns);
            if (mode == 2)
            {
                Color[] oracle;
                if (!buffers.TryTake(out oracle))
                    oracle = new Color[16384];
                var other = clone.Invoke(__instance, null);
                output.SetValue(other, oracle);
                long start = Stopwatch.GetTimestamp();
                OriginalColumn(other, x);
                Interlocked.Add(ref OriginalTicks, Stopwatch.GetTimestamp() - start);
                __state.Oracle = oracle;
                __state.Output = result;
            }

            __state.Start = Stopwatch.GetTimestamp();
            return true;
        }

        public static unsafe void End(int x, ColumnState __state)
        {
            if (__state.Start == 0)
                return;
            long ticks = Stopwatch.GetTimestamp() - __state.Start;
            if (mode == 1)
            {
                Interlocked.Add(ref OriginalTicks, ticks);
                return;
            }

            Interlocked.Add(ref FastTicks, ticks);
            Interlocked.Add(ref RemovedLerps, 2L * (16384 - 1));
            if (__state.Oracle == null)
                return;
            bool mismatch = false;
            fixed (Color* actual = __state.Output, oracle = __state.Oracle)
            {
                var a = (uint*)actual;
                var b = (uint*)oracle;
                for (int y = 0; y < 128; y++)
                    for (int c = 0; c < 4; c++)
                    {
                        int index = 4 * (x + y * 128) + c;
                        if (a[index] != b[index])
                            mismatch = true;
                    }

                if (mismatch)
                    for (int y = 0; y < 128; y++)
                        for (int c = 0; c < 4; c++)
                        {
                            int index = 4 * (x + y * 128) + c;
                            a[index] = b[index];
                        }
            }

            Interlocked.Increment(ref Checks);
            if (mismatch)
            {
                Interlocked.Increment(ref Mismatches);
                Interlocked.Exchange(ref rejected, 1);
            }

            buffers.Add(__state.Oracle);
        }

        public static void BakeBegin(out long __state)
        {
            Interlocked.Exchange(ref compatible, critical.All(m => Harmony.GetPatchInfo(m) == null) ? 1 : 0);
            __state = Stopwatch.GetTimestamp();
        }

        public static Exception BakeEnd(long __state, Exception __exception)
        {
            if (emit != null)
                emit(
                    "EVE_CURVE_BAKE",
                    "seconds=" + ((Stopwatch.GetTimestamp() - __state) / (double)Stopwatch.Frequency).ToString("F6", System.Globalization.CultureInfo.InvariantCulture)
                    + "; exception=" + (__exception == null ? "none" : __exception.GetType().Name)
                );
            return __exception;
        }

        public static void Report()
        {
            if (emit != null)
                emit(
                    "EVE_CURVE_COUNTERS",
                    "mode=" + mode
                    + "; columns=" + Columns
                    + "; checks=" + Checks
                    + "; mismatches=" + Mismatches
                    + "; fallback=" + Fallback
                    + "; removedLerps=" + RemovedLerps
                    + "; originalSeconds=" + (OriginalTicks / (double)Stopwatch.Frequency).ToString("F6", System.Globalization.CultureInfo.InvariantCulture)
                    + "; fastSeconds=" + (FastTicks / (double)Stopwatch.Frequency).ToString("F6", System.Globalization.CultureInfo.InvariantCulture)
                    + "; rejected=" + rejected
                );
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source, ILGenerator generator)
        {
            var code = source.Select(c => new CodeInstruction(c)).ToList();
            var offsets = Offsets(target.GetMethodBody().GetILAsByteArray());
            if (offsets.Count != code.Count)
                throw new NotSupportedException("Another patch changed the curve-baker instructions this optimization replaces.");
            var map = offsets.Select((offset, i) => new { offset, i }).ToDictionary(x => x.offset, x => x.i);
            int first = map[0x91], last = map[0xef], density = map[0x134], densityEnd = map[0x166], insert = map[0x42];
            var moved = code.GetRange(first, last - first).Concat(code.GetRange(density, densityEnd - density)).ToList();
            if (moved.Any(c => c.labels.Count != 0 || c.blocks.Count != 0) || code[insert].labels.Count != 0)
                throw new NotSupportedException("Curve hoist crosses a branch");
            var local = generator.DeclareLocal(typeof(float));
            var prefix = new List<CodeInstruction>();
            prefix.AddRange(code.GetRange(first, last - first).Select(c => new CodeInstruction(c)));
            prefix.AddRange(code.GetRange(density, densityEnd - density).Select(c => new CodeInstruction(c)));
            prefix.Add(new CodeInstruction(OpCodes.Stloc, local));
            for (int i = 0; i < code.Count; i++)
            {
                if (i == insert)
                    foreach (var instruction in prefix)
                        yield return instruction;
                if (i == density)
                {
                    yield return new CodeInstruction(OpCodes.Ldloc, local);
                    i = densityEnd - 1;
                    continue;
                }

                if (i == first)
                {
                    i = last - 1;
                    continue;
                }

                yield return code[i];
            }
        }

        static List<int> Offsets(byte[] bytes)
        {
            var ops = typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static)
                .Where(f => f.FieldType == typeof(OpCode))
                .Select(f => (OpCode)f.GetValue(null))
                .ToDictionary(o => o.Value);
            var result = new List<int>();
            int p = 0;
            while (p < bytes.Length)
            {
                result.Add(p);
                short value = bytes[p++];
                if (value == 0xfe)
                    value = (short)(0xfe00 | bytes[p++]);
                switch (ops[value].OperandType)
                {
                    case OperandType.InlineNone:
                        break;
                    case OperandType.ShortInlineBrTarget:
                    case OperandType.ShortInlineI:
                    case OperandType.ShortInlineVar:
                        p++;
                        break;
                    case OperandType.InlineVar:
                        p += 2;
                        break;
                    case OperandType.InlineI8:
                    case OperandType.InlineR:
                        p += 8;
                        break;
                    case OperandType.InlineSwitch:
                        int count = BitConverter.ToInt32(bytes, p);
                        p += 4 + 4 * count;
                        break;
                    default:
                        p += 4;
                        break;
                }
            }

            if (p != bytes.Length)
                throw new NotSupportedException("Invalid curve IL length");
            return result;
        }
    }
}
