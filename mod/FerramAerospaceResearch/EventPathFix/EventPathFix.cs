using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.Serialization;
using HarmonyLib;
using UnityEngine;

namespace KoeFarEventPath
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        static Action<Part> createRendererLists;
        static MethodInfo createMethod;
        static IComparer<RaycastHit> comparer;
        static FieldInfo validParts;
        static MethodInfo fairingHandler;
        static EventData<Part>.OnEvent statelessHandler;
        static FieldInfo eventItems, eventOwner;
        public void Awake()
        {
            Install("FAR.RendererInitDispatch", SetupRenderer);
            Install("FAR.HitSortBuffers", SetupSort);
            Install("FAR.FairingLifecycle", SetupFairings);
            Install("FAR.ColliderOwnerIndex", ColliderOwners.Install);
            ExposurePool.Apply();
            RustRendererFilter.Apply();
            RustRendererSetup.Apply();
            Destroy(this);
        }

        static Type Target(string name)
        {
            var type = AccessTools.TypeByName(name);
            if (type == null || type.Assembly.ManifestModule.ModuleVersionId != new Guid("4c2950d0-b874-40bb-9f5c-1800283c5f75"))
                throw new NotSupportedException("A FAR type is missing or belongs to an unsupported assembly: " + name);
            return type;
        }

        static void Install(string key, Action<Harmony> apply)
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled(key))
                return;
            var harmony = new Harmony("KerbalOverEngineering." + key);
            try
            {
                apply(harmony);
                Debug.Log("[KOE FAR] Applied " + key);
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(harmony.Id);
                Debug.LogError("[KOE FAR] Disabled " + key + ": " + ex.Message);
            }
        }

        static void SetupRenderer(Harmony harmony)
        {
            var type = Target("FerramAerospaceResearch.FARAeroComponents.VehicleExposure");
            createMethod = AccessTools.DeclaredMethod(typeof(Part), "CreateRendererLists", Type.EmptyTypes);
            if (createMethod == null || createMethod.ReturnType != typeof(void))
                throw new NotSupportedException("Cannot find FAR's renderer-list initialization method.");
            var dm = new DynamicMethod("KOE_CreateRenderers", typeof(void), new[] { typeof(Part) }, typeof(Bootstrap), true);
            var il = dm.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Call, createMethod);
            il.Emit(OpCodes.Ret);
            createRendererLists = (Action<Part>)dm.CreateDelegate(typeof(Action<Part>));
            // AI-generated comment: ResetParts is generic; closed instantiations share runtime code.
            // Patch its concrete reflection invocation through the two callers'
            // instantiated method bodies only when Harmony supports that shape.
            var reset = AccessTools.DeclaredMethod(type, "ResetParts").MakeGenericMethod(typeof(List<Part>));
            if (Harmony.GetPatchInfo(reset) != null)
                throw new NotSupportedException("Exposure reset already patched");
            harmony.Patch(reset, null, null, new HarmonyMethod(typeof(Bootstrap), "RendererDispatch"));
        }

        public static IEnumerable<CodeInstruction> RendererDispatch(IEnumerable<CodeInstruction> source)
        {
            int count = 0;
            foreach (var instruction in source)
            {
                var method = instruction.operand as MethodInfo;
                if (method != null
                    && method.Name == "Invoke"
                    && method.DeclaringType == typeof(MethodBase)
                    && method.GetParameters().Length == 2)
                {
                    var copy = new CodeInstruction(instruction);
                    copy.opcode = OpCodes.Call;
                    copy.operand = AccessTools.Method(typeof(Bootstrap), "InvokeRendererInit");
                    count++;
                    yield return copy;
                }
                else
                    yield return instruction;
            }

            if (count != 1)
                throw new NotSupportedException("Cannot find the reflection call used to initialize FAR exposure renderers.");
        }

        public static object InvokeRendererInit(MethodBase method, object instance, object[] arguments)
        {
            if (!Equals(method, createMethod) || !(instance is Part) || arguments == null || arguments.Length != 0)
                return method.Invoke(instance, arguments);
            try
            {
                createRendererLists((Part)instance);
                return null;
            }
            catch (Exception error)
            {
                throw new TargetInvocationException(error);
            }
        }

        static void SetupSort(Harmony harmony)
        {
            var type = Target("ferram4.FARWingInteraction");
            comparer = (IComparer<RaycastHit>)AccessTools.Field(type, "_comparer").GetValue(null);
            var method = AccessTools.DeclaredMethod(type, "SortHitsByDistance", new[] { typeof(RaycastHit[]) });
            if (method == null || method.ReturnType != typeof(RaycastHit[]) || Harmony.GetPatchInfo(method) != null)
                throw new NotSupportedException("The FAR hit-sort method is missing or already has a Harmony patch.");
            harmony.Patch(method, new HarmonyMethod(typeof(Bootstrap), "Sort"));
        }

        static bool Sort(RaycastHit[] __0, ref RaycastHit[] __result)
        {
            __result = SortedCopy(__0, comparer);
            return false;
        }

        public static RaycastHit[] SortedCopy(RaycastHit[] source, IComparer<RaycastHit> order)
        {
            if (source == null)
                throw new ArgumentNullException("source");
            if (source.Length == 0)
                return new RaycastHit[0];
            var result = (RaycastHit[])source.Clone();
            Array.Sort(result, order);
            return result;
        }

        static void SetupFairings(Harmony harmony)
        {
            var type = Target("FerramAerospaceResearch.FARPartGeometry.GeometryModification.StockProcFairingGeoUpdater");
            validParts = AccessTools.Field(type, "validParts");
            fairingHandler = AccessTools.DeclaredMethod(type, "FairingDeployGeometryUpdate", new[] { typeof(Part) });
            if (validParts == null || fairingHandler == null)
                throw new NotSupportedException("Cannot find FAR's fairing-geometry event handler.");
            foreach (var instruction in PatchProcessor.GetOriginalInstructions(fairingHandler))
                if (instruction.opcode == OpCodes.Ldarg_0)
                    throw new NotSupportedException("Fairing handler now uses instance state");
            // AI-generated comment: The original handler reads only the static dictionary. Bind it to a
            // stateless instance instead of retaining the first actual fairing.
            var empty = FormatterServices.GetUninitializedObject(type);
            statelessHandler = (EventData<Part>.OnEvent)Delegate.CreateDelegate(typeof(EventData<Part>.OnEvent), empty, fairingHandler);
            eventItems = AccessTools.Field(typeof(EventData<Part>), "events");
            if (eventItems == null || !typeof(IList).IsAssignableFrom(eventItems.FieldType))
                throw new NotSupportedException("The game event no longer exposes the expected listener list.");
            var entryType = eventItems.FieldType.GetGenericArguments()[0];
            eventOwner = AccessTools.Field(entryType, "originator");
            if (eventOwner == null)
                throw new NotSupportedException("The game event listener does not have the expected instance target.");
            var constructors = AccessTools.GetDeclaredConstructors(type);
            if (constructors.Count != 1)
                throw new NotSupportedException("Cannot find the FAR fairing-updater constructors expected by this patch.");
            var destroy = AccessTools.DeclaredMethod(Target("FerramAerospaceResearch.FARPartGeometry.GeometryPartModule"), "OnDestroy", Type.EmptyTypes);
            if (destroy == null || Harmony.GetPatchInfo(destroy) != null || Harmony.GetPatchInfo(constructors[0]) != null)
                throw new NotSupportedException("Fairing lifecycle already patched");
            harmony.Patch(constructors[0], null, new HarmonyMethod(typeof(Bootstrap), "FairingCreated"));
            harmony.Patch(destroy, new HarmonyMethod(typeof(Bootstrap), "GeometryDestroyed"));
            if (validParts.GetValue(null) != null)
                RebindFairingHandler();
        }

        static void FairingCreated()
        {
            RebindFairingHandler();
        }

        static void RebindFairingHandler()
        {
            // AI-generated comment: Also handle a fairing created before this addon. Retain the original
            // listener position relative to other mods; never add a second FAR
            // dispatch or retain a real updater through the event target.
            var events = (IList)eventItems.GetValue(GameEvents.onFairingsDeployed);
            int first = -1;
            for (int i = 0; i < events.Count;)
            {
                var owner = eventOwner.GetValue(events[i]);
                if (owner != null && owner.GetType() == fairingHandler.DeclaringType)
                {
                    if (first < 0)
                        first = i;
                    events.RemoveAt(i);
                }
                else
                    i++;
            }

            GameEvents.onFairingsDeployed.Add(statelessHandler);
            if (first >= 0 && first < events.Count - 1)
            {
                var added = events[events.Count - 1];
                events.RemoveAt(events.Count - 1);
                events.Insert(first, added);
            }
        }

        static void GeometryDestroyed(PartModule __instance)
        {
            var dictionary = validParts.GetValue(null) as IDictionary;
            if (dictionary != null
                && !ReferenceEquals(__instance.part, null)
                && dictionary.Contains(__instance.part)
                && ReferenceEquals(dictionary[__instance.part], __instance))
                dictionary.Remove(__instance.part);
        }
    }
}
