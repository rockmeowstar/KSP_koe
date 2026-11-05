using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeFreeIvaRefresh
{
    // AI-generated comment: Lifecycle resets prevent a disabled/re-enabled hatch from inheriting the
    // previous pending request. No Update method or per-frame polling is added.
    public sealed class ConnectionEpoch : MonoBehaviour
    {
        public int Frame = -1;
        void OnDisable()
        {
            Frame = -1;
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            const string buffers = "FreeIva.RendererVisitBuffers";
            if (KerbalOverEngineering.Options.Settings.Enabled(buffers))
            {
                var separate = new Harmony("KerbalOverEngineering." + buffers);
                try
                {
                    RendererVisitBuffers.Install(separate);
                    Debug.Log("[KOE FreeIVA] Fresh renderer traversal retained; buffers reused and material getter hoisted per renderer");
                }
                catch (Exception ex)
                {
                    separate.UnpatchAll(separate.Id);
                    Debug.LogError("[KOE FreeIVA] Renderer buffers disabled: " + ex.Message);
                }
            }

            const string foreign = "FreeIva.ForeignVesselRefresh";
            if (KerbalOverEngineering.Options.Settings.Enabled(foreign))
            {
                var separate = new Harmony("KerbalOverEngineering." + foreign);
                try
                {
                    ForeignRefresh.Install(separate);
                    Debug.Log("[KOE FreeIVA] Foreign-vessel refresh scoped; foreign cleanup retained");
                }
                catch (Exception ex)
                {
                    separate.UnpatchAll(separate.Id);
                    Debug.LogError("[KOE FreeIVA] Foreign refresh disabled: " + ex.Message);
                }
            }

            const string key = "FreeIva.HatchConnectionDedup";
            if (!KerbalOverEngineering.Options.Settings.Enabled(key))
            {
                Destroy(this);
                return;
            }

            var harmony = new Harmony("KerbalOverEngineering." + key);
            try
            {
                var type = AccessTools.TypeByName("FreeIva.FreeIvaHatch");
                if (type == null || type.Assembly.ManifestModule.ModuleVersionId != new Guid("44f85c42-e554-4c92-842c-629cde208456"))
                    throw new NotSupportedException("FreeIVA must match the 0.2.20.2 build this patch supports.");
                var method = AccessTools.DeclaredMethod(type, "RefreshConnection", Type.EmptyTypes);
                if (method == null || Harmony.GetPatchInfo(method) != null)
                    throw new NotSupportedException("Hatch method missing/already patched");
                harmony.Patch(method, null, null, new HarmonyMethod(typeof(Bootstrap), "Transpile"));
                Debug.Log("[KOE FreeIVA] Hatch connection scheduling coalesced before next-frame evaluation; immediate activation retained");
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(harmony.Id);
                Debug.LogError("[KOE FreeIVA] Disabled: " + ex.Message);
            }

            Destroy(this);
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source)
        {
            int edits = 0;
            var start = AccessTools.Method(typeof(MonoBehaviour), "StartCoroutine", new[] { typeof(IEnumerator) });
            foreach (var instruction in source)
            {
                if (instruction.Calls(start))
                {
                    var copy = new CodeInstruction(instruction);
                    copy.opcode = OpCodes.Call;
                    copy.operand = AccessTools.Method(typeof(Bootstrap), "StartConnection");
                    edits++;
                    yield return copy;
                }
                else
                    yield return instruction;
            }

            if (edits != 1)
                throw new NotSupportedException("RefreshConnection must start exactly one CheckForConnection coroutine.");
        }

        public static Coroutine StartConnection(MonoBehaviour owner, IEnumerator routine)
        {
            // AI-generated comment: CheckForConnection starts with yield return null. Requests made in
            // this same frame all read the final live hatch state on resumption.
            if (!owner.gameObject.activeInHierarchy)
                return owner.StartCoroutine(routine);
            var epoch = owner.GetComponent<ConnectionEpoch>();
            if (epoch == null)
                epoch = owner.gameObject.AddComponent<ConnectionEpoch>();
            if (epoch.Frame == Time.frameCount)
                return null;
            int old = epoch.Frame;
            epoch.Frame = Time.frameCount;
            try
            {
                return owner.StartCoroutine(routine);
            }
            catch
            {
                epoch.Frame = old;
                throw;
            }
        }
    }
}
