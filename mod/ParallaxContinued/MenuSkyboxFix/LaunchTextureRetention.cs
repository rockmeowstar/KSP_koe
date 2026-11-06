using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace MenuSkyboxFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class LaunchTextureRetention : MonoBehaviour
    {
        [ThreadStatic]
        static int launchDepth;
        [ThreadStatic]
        static Stack<string> restarts;
        static FieldInfo textures, planetName;
        static MethodInfo launch, restart, unload;
        public static long AvoidedUnloads, RetainedTextures;
        public static void Bind(Assembly assembly)
        {
            string problem = KerbalOverEngineering.Options.ManagedContract.ParallaxProblem(assembly, "LaunchTextureRetention");
            if (problem != null) throw new NotSupportedException(problem);
            var body = assembly.GetType("Parallax.ParallaxScatterBody", true);
            textures = AccessTools.Field(body, "loadedTextures");
            planetName = AccessTools.Field(body, "planetName");
            launch = AccessTools.DeclaredMethod(typeof(FlightDriver), "setStartupNewVessel", Type.EmptyTypes);
            restart = AccessTools.DeclaredMethod(assembly.GetType("Parallax.ScatterManager", true), "DominantBodyRestarted", new[] { typeof(string) });
            unload = AccessTools.DeclaredMethod(body, "UnloadTextures", Type.EmptyTypes);
            if (textures == null
                || textures.IsStatic
                || textures.FieldType != typeof(Dictionary<string, Texture>)
                || planetName == null
                || planetName.FieldType != typeof(string)
                || launch == null
                || restart == null
                || unload == null
                || launch.ReturnType != typeof(void)
                || restart.ReturnType != typeof(void)
                || unload.ReturnType != typeof(void))
                throw new NotSupportedException("Parallax launch texture API changed");
        }

        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("ParallaxContinued.LaunchTextureRetention"))
            {
                Destroy(this);
                return;
            }

            const string id = "local.kerbaloverengineering.parallax-launch-textures";
            var harmony = new Harmony(id);
            try
            {
                Bind(AppDomain.CurrentDomain.GetAssemblies().Single(a => a.GetName().Name == "ParallaxContinued"));
                harmony.Patch(
                    launch,
                    new HarmonyMethod(typeof(LaunchTextureRetention), "BeginLaunch"),
                    null,
                    null,
                    new HarmonyMethod(typeof(LaunchTextureRetention), "EndLaunch")
                );
                harmony.Patch(
                    restart,
                    new HarmonyMethod(typeof(LaunchTextureRetention), "BeginRestart"),
                    null,
                    null,
                    new HarmonyMethod(typeof(LaunchTextureRetention), "EndRestart")
                );
                harmony.Patch(unload, new HarmonyMethod(typeof(LaunchTextureRetention), "Retain"));
                Debug.Log("[MenuSkyboxFix] Retain existing scatter textures only during same-body restarts inside new-vessel startup.");
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(id);
                Debug.LogError("[MenuSkyboxFix] Launch texture retention disabled: " + ex.Message);
            }

            Destroy(this);
        }

        internal static void BeginLaunch(out bool __state)
        {
            __state = true;
            launchDepth++;
        }

        internal static Exception EndLaunch(Exception __exception, bool __state)
        {
            if (__state)
                launchDepth--;
            return __exception;
        }

        internal static void BeginRestart(string bodyName, out string __state)
        {
            __state = launchDepth > 0 && !string.IsNullOrEmpty(bodyName) ? bodyName : null;
            if (__state != null)
            {
                if (restarts == null)
                    restarts = new Stack<string>();
                restarts.Push(__state);
            }
        }

        internal static Exception EndRestart(Exception __exception, string __state)
        {
            if (__state != null && restarts != null && restarts.Count > 0)
                restarts.Pop();
            return __exception;
        }

        internal static bool InScope(string body)
        {
            return launchDepth > 0
                && restarts != null
                && restarts.Count > 0
                && string.Equals(restarts.Peek(), body, StringComparison.Ordinal);
        }

        static bool Retain(object __instance)
        {
            if (!InScope((string)planetName.GetValue(__instance)))
                return true;
            var loaded = (Dictionary<string, Texture>)textures.GetValue(__instance);
            if (loaded == null || loaded.Count == 0 || loaded.Values.Any(t => t == null))
                return true;
            // AI-generated comment: Renderer/shader/biome/PQS restart still runs. Its normal texture lookup reuses this same dictionary.
            // The next ordinary body unload destroys these textures as before; no cross-body retention/cache.
            AvoidedUnloads++;
            RetainedTextures += loaded.Count;
            return false;
        }
    }
}
