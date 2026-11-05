using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.IO;
using Firefly;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

[assembly: AssemblyTitle("KerbalOverEngineering Firefly Idle Fix")]
[assembly: AssemblyVersion("1.2.0.0")]
namespace FireflyIdleFix
{
    // AI-generated comment: Nonpositive strength is clamped to zero by glow; both geometry passes return.
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string PluginSha = "e312eede3f5f4837aadae302c721f8cbd67222b8722a5f01a5899f29dc288829";
        // AI-generated comment: 1.0.6 keeps the same rendering bundle. Its envelope draw commands and
        // strength/depth paths were compared against 1.0.5; only logging and
        // null-safe override-config initialization changed in AtmoFxModule.
        const string Plugin106Sha = "91e11e3fbc41c3e56cf40745e1af7360811cf1892cf18d3bda8e4db3b9eb35ba";
        const string BundleSha = "abe1db2b133decab1c879ac03660dee919b3e245b6c659883a0ddd527ce6be28";
        static readonly ConditionalWeakTable<AtmoFxModule, State> states = new ConditionalWeakTable<AtmoFxModule, State>();
        static PropertyInfo managerInstance;
        static MethodInfo addBuffer, removeBuffer;
        static long skipped, active;
        static bool failed;
        sealed class State
        {
            public Camera Camera;
            public CommandBuffer Buffer;
            public bool CameraEnabled;
            public object Manager;
        }

        static bool Matches(string path, string expected)
        {
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant() == expected;
        }

        public void Awake()
        {
            bool zero = KerbalOverEngineering.Options.Settings.Enabled("Firefly.ZeroStrengthGate");
            bool depth = KerbalOverEngineering.Options.Settings.Enabled("Firefly.DepthLighting");
            bool globals = KerbalOverEngineering.Options.Settings.Enabled("Firefly.GlobalCommandDedup");
            if (!zero && !depth && !globals)
            {
                Destroy(this);
                return;
            }

            if (Environment.GetEnvironmentVariable("KOE_FIREFLY_IDLE_DISABLE") == "1")
            {
                Destroy(this);
                return;
            }

            try
            {
                string root = Path.Combine(KSPUtil.ApplicationRootPath, "GameData/Firefly");
                if (!(Matches(typeof(AtmoFxModule).Assembly.Location, PluginSha)
                    || Matches(typeof(AtmoFxModule).Assembly.Location, Plugin106Sha))
                    || !Matches(Path.Combine(root, "Assets/fireflybundle.ksp"), BundleSha))
                    throw new NotSupportedException("Firefly plugin/bundle identity mismatch");
                Type manager = typeof(AtmoFxModule).Assembly.GetType("Firefly.CameraManager", true);
                managerInstance = manager.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
                addBuffer = AccessTools.Method(manager, "AddCommandBuffer");
                removeBuffer = AccessTools.Method(manager, "RemoveCommandBuffer");
                if (managerInstance == null || addBuffer == null || removeBuffer == null)
                    throw new MissingMemberException("CameraManager");
                var harmony = new Harmony("koe.firefly.zero-output.1");
                if (zero)
                {
                    harmony.Patch(AccessTools.Method(typeof(AtmoFxModule), "UpdateMaterialProperties"), transpiler: new HarmonyMethod(typeof(Bootstrap), "InsertGate"));
                    harmony.Patch(AccessTools.Method(typeof(AtmoFxModule), "DestroyCommandBuffer"), prefix: new HarmonyMethod(typeof(Bootstrap), "Destroying"));
                }

                if (globals)
                    EnvelopeCommands.Bind(harmony);
                if (depth && DepthLighting.Bind(harmony))
                    StartCoroutine(DepthLighting.EndFrames());
                DontDestroyOnLoad(gameObject);
                Debug.Log("[FireflyIdleFix] Loaded 1.2.0: zero-output idle gate; scoped depth-camera lighting; coalesced envelope globals");
            }
            catch (Exception ex)
            {
                Debug.LogError("[FireflyIdleFix] Disabled: " + ex);
                Destroy(this);
            }
        }

        static IEnumerable<CodeInstruction> InsertGate(IEnumerable<CodeInstruction> instructions, ILGenerator generator)
        {
            var code = instructions.ToList();
            var strength = AccessTools.Method(typeof(AtmoFxModule), "GetEntryStrength");
            if (code.Count < 4 || code[0].opcode != OpCodes.Ldarg_0 || !code[1].Calls(strength) || code[2].opcode != OpCodes.Stloc_0)
                throw new NotSupportedException("Cannot find Firefly's expected strength calculation; using its original method.");
            Label resume = generator.DefineLabel();
            code[3].labels.Add(resume);
            code.InsertRange(
                3,
                new[] {
                new CodeInstruction(OpCodes.Ldarg_0),
                new CodeInstruction(OpCodes.Ldloc_0),
                new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(Bootstrap), "ZeroOutput")),
                new CodeInstruction(OpCodes.Brfalse, resume),
                new CodeInstruction(OpCodes.Ret)
            }
            );
            return code;
        }

        static void Restore(AtmoFxModule module)
        {
            State s;
            if (!states.TryGetValue(module, out s))
                return;
            try
            {
                addBuffer.Invoke(s.Manager, new object[] { CameraEvent.AfterForwardAlpha, s.Buffer });
            }
            finally
            {
                if (s.Camera != null)
                    s.Camera.enabled = s.CameraEnabled;
            }

            states.Remove(module);
            Debug.Log("[FireflyIdleFix] Restored rendering");
        }

        static bool ZeroOutput(AtmoFxModule module, float strength)
        {
            try
            {
                var fx = module.fxVessel;
                if (failed
                    || !module.isLoaded
                    || fx == null
                    || module.OverridePhysics
                    || !(strength <= 0f)
                    || fx.airstreamCamera == null
                    || fx.commandBuffer == null
                    || fx.material == null
                    || fx.material.shader == null
                    || fx.material.shader.name != "Firefly/Firefly")
                {
                    Restore(module);
                    active++;
                    return false;
                }

                // AI-generated comment: Residual particles keep sampling the depth camera until their natural expiry.
                if (fx.hasParticles)
                {
                    if (!fx.areParticlesKilled)
                    {
                        Restore(module);
                        active++;
                        return false;
                    }

                    foreach (var p in fx.allParticles.Values)
                        if (p.system != null && p.system.IsAlive(true))
                        {
                            Restore(module);
                            active++;
                            return false;
                        }
                }

                State s;
                if (!states.TryGetValue(module, out s))
                {
                    object manager = managerInstance.GetValue(null, null);
                    if (manager == null || FlightCamera.fetch == null || FlightCamera.fetch.mainCamera == null)
                        return false;
                    s = new State
                    {
                        Camera = fx.airstreamCamera,
                        CameraEnabled = fx.airstreamCamera.enabled,
                        Buffer = fx.commandBuffer,
                        Manager = manager
                    };
                    states.Add(module, s);
                    removeBuffer.Invoke(manager, new object[] { CameraEvent.AfterForwardAlpha, s.Buffer });
                    s.Camera.enabled = false;
                    Debug.Log("[FireflyIdleFix] Suspended zero-output rendering");
                }

                skipped++;
                return true;
            }
            catch (Exception ex)
            {
                failed = true;
                try
                {
                    Restore(module);
                }
                catch
                {
                }

                Debug.LogError("[FireflyIdleFix] Original path restored after error: " + ex);
                return false;
            }
        }

        // AI-generated comment: Reload disposes/replaces the buffer but can reuse the camera. Do not reattach a dying buffer.
        static void Destroying(AtmoFxModule __instance)
        {
            State s;
            if (!states.TryGetValue(__instance, out s))
                return;
            if (s.Camera != null)
                s.Camera.enabled = s.CameraEnabled;
            states.Remove(__instance);
        }

        public void OnApplicationQuit()
        {
            Debug.Log("[FireflyIdleFix] Counters: zero-output=" + skipped + " original=" + active);
        }

        public void LateUpdate()
        {
            DepthLighting.FrameStart();
        }

        public void OnDestroy()
        {
            DepthLighting.Stop();
        }
    }
}
