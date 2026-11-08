using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoePlanetLaunchReadiness
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
#if KOE_DIAGNOSTICS
        [Serializable] public sealed class Report { public int readyEvents, shortened, originalWaits; public bool complete; public string[] decisions; }
        const string Id = "local.koe.launch-readiness-trial";
#else
        const string Id = "KerbalOverEngineering.Stock.PlanetLaunchReadiness";
#endif
        static readonly HashSet<PQS> ready = new HashSet<PQS>();
        static readonly HashSet<PQS> built = new HashSet<PQS>();
        static FieldInfo sphereField, siteField;
        static bool applied;
#if KOE_DIAGNOSTICS
        static readonly List<string> decisions = new List<string>();
        static int readyEvents, shortened, originalWaits;
        public static int LastStep = -1;
        static string[] tracedInstructions;
#endif
        public void Awake()
        {
#if KOE_DIAGNOSTICS
            if (Environment.GetEnvironmentVariable("KOE_LAUNCH_READY_TRIAL") != "1") { Destroy(this); return; }
#else
            if (!KerbalOverEngineering.Options.Settings.Enabled("Stock.PlanetLaunchReadiness")) { Destroy(this); return; }
#endif
            var harmony = new Harmony(Id);
            try
            {
                var iterator = typeof(PSystemSetup).GetNestedTypes(BindingFlags.Public | BindingFlags.NonPublic).Single(t => t.Name.StartsWith("<SetupLaunchSites>", StringComparison.Ordinal));
                BindFields(iterator);
                var target = AccessTools.DeclaredMethod(iterator, "MoveNext");
                var force = AccessTools.DeclaredMethod(typeof(PSystemSetup), "ForceInitPQS", new[] { typeof(PQS), typeof(Transform) });
                var quads = AccessTools.DeclaredMethod(typeof(PQS), "UpdateQuadsInit");
                foreach (var method in new[] { target, force, quads })
                {
                    var patches = Harmony.GetPatchInfo(method);
                    if (patches != null && patches.Owners.Any(ForeignOwner))
                        throw new NotSupportedException("Launch preparation already patched by another adapter");
                }
                harmony.Patch(force, new HarmonyMethod(typeof(Bootstrap), "BeforeForce"), null, null, new HarmonyMethod(typeof(Bootstrap), "AfterForce"));
                harmony.Patch(quads, null, new HarmonyMethod(typeof(Bootstrap), "AfterQuads"));
                harmony.Patch(target, null, null, new HarmonyMethod(typeof(Bootstrap), "Rewrite"));
#if KOE_DIAGNOSTICS
                if (Environment.GetEnvironmentVariable("KOE_LAUNCH_READY_TRACE") == "1")
                    harmony.Patch(target, new HarmonyMethod(typeof(Bootstrap), "TraceBefore"), null,
                        new HarmonyMethod(typeof(Bootstrap), "Trace") { priority = Priority.Last }, new HarmonyMethod(typeof(Bootstrap), "TraceFailure"));
#endif
                applied = true; DontDestroyOnLoad(gameObject);
                Debug.Log("[KOE LaunchReadiness] Completed initial quads, ForceInitPQS return and physics boundary required; raycast sites keep stock waits");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); ready.Clear(); built.Clear(); Debug.LogError("[KOE LaunchReadiness] Disabled: " + error.Message); Destroy(this); }
        }
        static bool ForeignOwner(string owner)
        {
#if KOE_DIAGNOSTICS
            return owner != "local.koe.planet-startup-metrics";
#else
            return true;
#endif
        }
        static void BindFields(Type iterator)
        {
            var fields = iterator.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            sphereField = fields.Single(f => f.FieldType == typeof(PQS));
            siteField = fields.Single(f => f.FieldType == typeof(LaunchSite));
        }
        public static bool CanShorten(bool completed, bool started, bool alive, bool needsRaycast)
        {
            return completed && started && alive && !needsRaycast;
        }
        static bool IsReady(PQS sphere)
        {
            return sphere != null && CanShorten(ready.Contains(sphere), sphere.isStarted, sphere.isAlive, false);
        }
        public static void BeforeForce(PQS __0)
        {
            if (__0 == null) return;
            ready.Remove(__0); built.Remove(__0);
        }
        public static void AfterQuads(PQS __instance) { built.Add(__instance); }
        public static Exception AfterForce(PQS __0, Exception __exception)
        {
            if (__0 != null)
            {
                if (__exception == null && built.Remove(__0))
                {
                    ready.Add(__0);
#if KOE_DIAGNOSTICS
                    readyEvents++;
#endif
                }
                else { ready.Remove(__0); built.Remove(__0); }
            }
            return __exception;
        }
        public static object Wait(float seconds, object iterator)
        {
            try { return ChooseWait(seconds, iterator); }
            catch (Exception error)
            {
                Debug.LogError("[KOE LaunchReadiness] Using stock wait after adapter error: " + error);
#if KOE_DIAGNOSTICS
                originalWaits++;
#endif
                return new WaitForSeconds(seconds);
            }
        }
        static object ChooseWait(float seconds, object iterator)
        {
            var sphere = sphereField.GetValue(iterator) as PQS;
            var site = siteField.GetValue(iterator) as LaunchSite;
            bool raycast = site == null || (site.pqsCity2 != null && site.pqsCity2.snapToSurface && site.pqsCity2.raycastSurface);
#if KOE_DIAGNOSTICS
            decisions.Add("site=" + (site == null ? "unknown" : site.name) + "; raycast=" + raycast + "; sphere=" + (sphere != null) + "; marker=" + ready.Contains(sphere) + "; started=" + (sphere != null && sphere.isStarted) + "; alive=" + (sphere != null && sphere.isAlive) + "; target=" + (sphere != null && sphere.target != null) + "; visible=" + (sphere != null && sphere.isActive));
#endif
            if (float.IsNaN(seconds) || float.IsInfinity(seconds) || seconds <= Time.fixedDeltaTime || raycast || !IsReady(sphere))
            {
#if KOE_DIAGNOSTICS
                originalWaits++;
#endif
                return new WaitForSeconds(seconds);
            }
            return AfterPhysics(sphere, seconds);
        }
        static IEnumerator AfterPhysics(PQS sphere, float seconds)
        {
            yield return new WaitForFixedUpdate();
            if (!IsReady(sphere))
            {
#if KOE_DIAGNOSTICS
                originalWaits++;
#endif
                yield return new WaitForSeconds(seconds);
            }
#if KOE_DIAGNOSTICS
            else shortened++;
#else
            else Debug.Log("[KOE LaunchReadiness] Terrain ready; stock launch-site wait replaced");
#endif
            ready.Remove(sphere);
        }
#if KOE_DIAGNOSTICS
        public static void TraceBefore() { LastStep = -1; }
        public static Exception TraceFailure(object __instance, Exception __exception)
        {
            if (__exception == null) return null;
            var lines = new List<string>();
            lines.Add("step=" + LastStep + "; instruction=" + (LastStep >= 0 && LastStep < tracedInstructions.Length ? tracedInstructions[LastStep] : "prefix"));
            lines.Add(__exception.ToString());
            foreach (var field in __instance.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                object value = field.GetValue(__instance);
                lines.Add(field.Name + ": " + (value == null ? "null" : value.GetType().FullName));
            }
            string text = string.Join("\n", lines.ToArray());
            var report = Environment.GetEnvironmentVariable("KOE_LAUNCH_READINESS_REPORT");
            if (!string.IsNullOrEmpty(report)) File.WriteAllText(report + ".fault.txt", text);
            Debug.LogError("[KOE LaunchReadiness] Iterator failure: " + text);
            return __exception;
        }
        public static IEnumerable<CodeInstruction> Trace(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(i => new CodeInstruction(i)).ToList();
            tracedInstructions = code.Select(i => i.ToString()).ToArray();
            var field = AccessTools.Field(typeof(Bootstrap), "LastStep");
            for (int i = 0; i < code.Count; i++)
            {
                var instruction = code[i];
                bool prefix = i > 0 && code[i - 1].opcode.OpCodeType == OpCodeType.Prefix;
                if (!prefix && (instruction.opcode == OpCodes.Ldfld || instruction.opcode == OpCodes.Stfld || instruction.opcode.FlowControl == FlowControl.Call))
                {
                    var marker = new CodeInstruction(OpCodes.Ldc_I4, i);
                    marker.labels.AddRange(instruction.labels); marker.blocks.AddRange(instruction.blocks);
                    instruction.labels.Clear(); instruction.blocks.Clear();
                    yield return marker;
                    yield return new CodeInstruction(OpCodes.Stsfld, field);
                }
                yield return instruction;
            }
        }
#endif
        // AI-generated comment: Only post-ForceInitPQS timers in this iterator are eligible.
        // Collider raycasts retain the original timer; readiness loss also falls back.
        public static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> source)
        {
            var code = source.Select(i => new CodeInstruction(i)).ToList();
            var constructor = AccessTools.Constructor(typeof(WaitForSeconds), new[] { typeof(float) });
            var helper = AccessTools.DeclaredMethod(typeof(Bootstrap), "Wait");
            int changed = 0;
            for (int i = 0; i < code.Count; i++)
            {
                var instruction = code[i];
                if (instruction.opcode != OpCodes.Newobj || !Equals(instruction.operand, constructor)) { yield return instruction; continue; }
                var destination = i + 1 < code.Count ? code[i + 1].operand as FieldInfo : null;
                if (i == 0 || code[i - 1].opcode != OpCodes.Ldc_R4 || destination == null || code[i + 1].opcode != OpCodes.Stfld || destination.FieldType != typeof(object))
                    throw new NotSupportedException("Launch timer no longer directly yields a constant-duration wait");
                int priorCall = i - 1;
                while (priorCall >= 0 && !(code[priorCall].operand is MethodInfo))
                {
                    var flow = code[priorCall].opcode.FlowControl;
                    if (flow == FlowControl.Branch || flow == FlowControl.Cond_Branch || flow == FlowControl.Return) break;
                    priorCall--;
                }
                var force = priorCall >= 0 ? code[priorCall].operand as MethodInfo : null;
                if (force == null || force.DeclaringType != typeof(PSystemSetup) || force.Name != "ForceInitPQS")
                    throw new NotSupportedException("Launch timer does not follow ForceInitPQS");
                var context = new CodeInstruction(OpCodes.Ldarg_0); context.labels.AddRange(instruction.labels); context.blocks.AddRange(instruction.blocks);
                yield return context;
                yield return new CodeInstruction(OpCodes.Call, helper);
                changed++;
            }
            if (changed != 2) throw new NotSupportedException("Expected the two launch-site timer sites; found " + changed);
        }
        void Finish()
        {
            if (!applied) return;
#if KOE_DIAGNOSTICS
            string path = Environment.GetEnvironmentVariable("KOE_LAUNCH_READINESS_REPORT");
            if (!string.IsNullOrEmpty(path)) File.WriteAllText(path, JsonUtility.ToJson(new Report { readyEvents = readyEvents, shortened = shortened, originalWaits = originalWaits, complete = true, decisions = decisions.ToArray() }, true));
#endif
            ready.Clear(); built.Clear(); new Harmony(Id).UnpatchAll(Id); applied = false;
            Debug.Log("[KOE LaunchReadiness] Startup patches removed");
        }
        public void Update() { if (applied && HighLogic.LoadedScene == GameScenes.MAINMENU) { Finish(); Destroy(this); } }
        public void OnApplicationQuit() { Finish(); }
        public void OnDestroy() { Finish(); }
    }
}
