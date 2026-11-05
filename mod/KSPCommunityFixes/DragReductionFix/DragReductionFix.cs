using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using HarmonyLib;
using KerbalOverEngineering.Options;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeDragReduction
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.KSPCF.DragReduction";
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Configure(IntPtr texture, IntPtr buffer);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate IntPtr EventAddress();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Status();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void Release();

        static Configure configure;
        static Status status;
        static Release release;
        static ComputeBuffer buffer;
        static uint[] partial;
        static CommandBuffer commands;
        static FieldInfo debuggerCopy;
        static Camera camera;
        static Texture2D texture;
        static bool valid, skipped, rejected, verified;
        static int hits, totalDrag, maximum, textureSize, renderDepth;
        static long faces, fallbacks;
        bool ownsResources;

        static Delegate Export(IntPtr library, string name, Type type)
        {
            IntPtr address = GetProcAddress(library, name);
            if (address == IntPtr.Zero) throw new EntryPointNotFoundException(name);
            return Marshal.GetDelegateForFunctionPointer(address, type);
        }

        public void Awake()
        {
            if (!Settings.Enabled("KSPCommunityFixes.DragGpuReduction")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11 || !SystemInfo.supportsComputeShaders)
                    throw new NotSupportedException("Direct3D 11 compute shaders are required.");
                Type type = AccessTools.TypeByName("KSPCommunityFixes.Performance.DragCubeGeneration");
                if (type == null || type.Module.ModuleVersionId != new Guid("62bb7f54-2099-4f40-9a34-377c091e8538"))
                    throw new NotSupportedException("The installed KSPCF drag renderer has not been reviewed.");
                var render = AccessTools.DeclaredMethod(type, "Render");
                var calculate = AccessTools.DeclaredMethod(type, "CalculateAerodynamics");
                foreach (var method in new[] { render, calculate })
                {
                    if (method == null) throw new MissingMethodException("KSPCF drag renderer changed.");
                    var patches = Harmony.GetPatchInfo(method);
                    if (patches != null && patches.Owners.Count > 0)
                        throw new NotSupportedException("Another mod patches KSPCF drag reduction.");
                }
                textureSize = (int)AccessTools.Field(type, "textureSize").GetRawConstantValue();
                Type debugger = AccessTools.TypeByName("KSPCommunityFixes.Performance.DragCubeDebugger");
                debuggerCopy = debugger == null ? null : AccessTools.Field(debugger, "requireTextureCopy");
                if (debuggerCopy == null || debuggerCopy.FieldType != typeof(bool))
                    throw new MissingFieldException("KSPCF drag debugger ownership is unavailable.");
                string path = Path.Combine(NativeAssets.Extract(typeof(Bootstrap).Assembly), "KoeDragReduction.dll");
                IntPtr library = LoadLibraryW(path);
                if (library == IntPtr.Zero) throw new InvalidOperationException("Native loader error " + Marshal.GetLastWin32Error());
                configure = (Configure)Export(library, "ConfigureDragReduction", typeof(Configure));
                status = (Status)Export(library, "GetDragReductionStatus", typeof(Status));
                release = (Release)Export(library, "ReleaseDragReduction", typeof(Release));
                var address = (EventAddress)Export(library, "GetDragReductionEvent", typeof(EventAddress));
                ownsResources = true;
                commands = new CommandBuffer { name = "KOE exact drag reduction" };
                commands.IssuePluginEvent(address(), 0);
                harmony.Patch(render, prefix: new HarmonyMethod(typeof(Bootstrap), "BeginFace"),
                    postfix: new HarmonyMethod(typeof(Bootstrap), "EndFace"),
                    transpiler: new HarmonyMethod(typeof(Bootstrap), "SkipRoundTrip"),
                    finalizer: new HarmonyMethod(typeof(Bootstrap), "FinishFace"));
                harmony.Patch(calculate, prefix: new HarmonyMethod(typeof(Bootstrap), "Calculate"),
                    postfix: new HarmonyMethod(typeof(Bootstrap), "VerifyFirstFace"));
                Debug.Log("[KOE DragReduction] Ready; first nonempty face uses CPU/GPU parity check before enabling GPU results.");
            }
            catch (Exception error)
            {
                harmony.UnpatchAll(Id);
                Debug.LogWarning("[KOE DragReduction] Original renderer retained: " + error.Message);
                Destroy(this);
            }
        }

        static void BeginFace(Camera camera, Texture2D output)
        {
            renderDepth++;
            Bootstrap.camera = camera;
            texture = output;
            valid = skipped = false;
        }

        static void EndFace(Camera camera, Texture2D output)
        {
            if (!verified && renderDepth == 1 && ReferenceEquals(output, texture))
                TryReduce(camera.targetTexture, output);
        }

        static Exception FinishFace(Exception __exception)
        {
            renderDepth--;
            if (__exception != null) valid = skipped = false;
            return __exception;
        }

        static bool TryReduce(RenderTexture input, Texture2D output)
        {
            valid = false;
            if (rejected || input == null || output == null || output.format != TextureFormat.ARGB32
                || input.width != textureSize || input.height != textureSize || output.width != textureSize || output.height != textureSize
                || (bool)debuggerCopy.GetValue(null)) { fallbacks++; return false; }
            try
            {
                int groups = ((input.width + 15) / 16) * ((input.height + 15) / 16);
                if (buffer == null || buffer.count != groups)
                {
                    if (buffer != null) buffer.Release();
                    buffer = new ComputeBuffer(groups, 16);
                    partial = new uint[groups * 4];
                    buffer.SetData(partial);
                }
                int result = configure(input.GetNativeTexturePtr(), buffer.GetNativeBufferPtr());
                if (result < 0) throw new InvalidOperationException("GPU resources: " + result.ToString("X8"));
                Graphics.ExecuteCommandBuffer(commands);
                buffer.GetData(partial);
                result = status();
                if (result < 0) throw new InvalidOperationException("GPU callback: " + result.ToString("X8"));
                hits = totalDrag = maximum = 0;
                for (int index = 0; index < partial.Length; index += 4)
                {
                    hits = checked(hits + (int)partial[index]);
                    totalDrag = checked(totalDrag + (int)partial[index + 1]);
                    maximum = Math.Max(maximum, (int)partial[index + 2]);
                }
                if (hits > textureSize * textureSize || maximum > 255 || totalDrag > hits * 255L)
                    throw new InvalidOperationException("GPU statistics are outside the byte image bounds.");
                faces++;
                valid = true;
                return true;
            }
            catch (Exception error)
            {
                rejected = true;
                fallbacks++;
                Debug.LogWarning("[KOE DragReduction] Original readback restored: " + error.Message);
                return false;
            }
        }

        static void Statistics(float cameraSize, float farClip, out float area, out float drag, out float depth)
        {
            area = drag = depth = 0;
            if (hits <= 0) return;
            // AI-generated comment: Keep KSPCF's float operation order; only the integer pixel reduction moves to GPU.
            float pixelArea = Mathf.Pow(2f * cameraSize / textureSize, 2f);
            area = pixelArea * hits;
            drag = totalDrag / (hits * 255f);
            depth = farClip * (maximum / 255f);
        }

        static bool Calculate(Texture2D __0, float __1, float __2, ref float __3, ref float __4, ref float __5)
        {
            if (!verified || !valid || !skipped || !ReferenceEquals(__0, texture)) return true;
            Statistics(__1, __2, out __3, out __4, out __5);
            return false;
        }

        static unsafe int Bits(float value) { return *(int*)&value; }

        static void VerifyFirstFace(Texture2D __0, float __1, float __2, float __3, float __4, float __5)
        {
            if (verified || !valid || !ReferenceEquals(__0, texture)) return;
            float area, drag, depth;
            Statistics(__1, __2, out area, out drag, out depth);
            if (Bits(area) != Bits(__3) || Bits(drag) != Bits(__4) || Bits(depth) != Bits(__5))
            {
                rejected = true;
                Debug.LogError("[KOE DragReduction] First-face parity failed; original CPU results retained.");
            }
            else if (hits > 0)
            {
                verified = true;
                Debug.Log("[KOE DragReduction] First-face parity passed; exact GPU reduction enabled.");
            }
        }

        static void ReadPixels(Texture2D output, Rect rectangle, int x, int y)
        {
            if (verified && renderDepth == 1 && ReferenceEquals(output, texture) && camera != null
                && TryReduce(camera.targetTexture, output)) { skipped = true; return; }
            valid = skipped = false;
            output.ReadPixels(rectangle, x, y);
        }

        static void Apply(Texture2D output)
        {
            if (!skipped || !ReferenceEquals(output, texture)) output.Apply();
        }

        static IEnumerable<CodeInstruction> SkipRoundTrip(IEnumerable<CodeInstruction> instructions)
        {
            int reads = 0, applies = 0;
            foreach (var instruction in instructions)
            {
                var copy = new CodeInstruction(instruction);
                var method = copy.operand as MethodInfo;
                if (method != null && method.DeclaringType == typeof(Texture2D) && method.Name == "ReadPixels" && method.GetParameters().Length == 3)
                { copy.opcode = OpCodes.Call; copy.operand = AccessTools.DeclaredMethod(typeof(Bootstrap), "ReadPixels"); reads++; }
                else if (method != null && method.DeclaringType == typeof(Texture2D) && method.Name == "Apply" && method.GetParameters().Length == 0)
                { copy.opcode = OpCodes.Call; copy.operand = AccessTools.DeclaredMethod(typeof(Bootstrap), "Apply"); applies++; }
                yield return copy;
            }
            if (reads != 1 || applies != 1) throw new NotSupportedException("The reviewed readback/upload pair changed.");
        }

        public void OnApplicationQuit()
        {
            Debug.Log("[KOE DragReduction] faces=" + faces + "; verified=" + verified + "; fallbacks=" + fallbacks + "; rejected=" + rejected);
        }

        public void OnDestroy()
        {
            if (!ownsResources) return;
            new Harmony(Id).UnpatchAll(Id);
            rejected = true;
            if (release != null) release();
            if (buffer != null) { buffer.Release(); buffer = null; }
            if (commands != null) { commands.Dispose(); commands = null; }
        }
    }
}
