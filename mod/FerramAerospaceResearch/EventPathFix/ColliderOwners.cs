using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeFarEventPath
{
    public static class ColliderOwners
    {
        internal sealed class Context
        {
            internal List<Part> Parts;
            internal bool Built, Disabled;
            internal readonly Dictionary<Collider, List<Part>> Map = new Dictionary<Collider, List<Part>>();
            internal readonly Stack<List<Part>> Lists = new Stack<List<Part>>();
            internal void Clear()
            {
                foreach (var list in Map.Values)
                {
                    list.Clear();
                    Lists.Push(list);
                }

                Map.Clear();
                Parts = null;
                Built = Disabled = false;
            }
        }

        public struct Scope
        {
            internal Context Previous;
            internal bool Created;
        }

        [ThreadStatic]
        static Context current;
        [ThreadStatic]
        static Stack<Context> pool;
        static readonly List<Part> empty = new List<Part>();
        static Type moduleType;
        static Func<Component, Collider[]> colliders;
        static Context Rent()
        {
            if (pool == null)
                pool = new Stack<Context>();
            return pool.Count == 0 ? new Context() : pool.Pop();
        }

        public static void Install(Harmony harmony)
        {
            moduleType = AccessTools.TypeByName("ferram4.FARPartModule");
            if (moduleType == null
                || moduleType.Assembly.ManifestModule.ModuleVersionId != new Guid("4c2950d0-b874-40bb-9f5c-1800283c5f75"))
                throw new NotSupportedException("Cannot find FAR's collider-owner lookup in the supported assembly.");
            var field = AccessTools.Field(moduleType, "partColliders");
            if (field == null || field.FieldType != typeof(Collider[]))
                throw new NotSupportedException("Cannot find the FAR collider cache fields expected by the owner index.");
            var dm = new DynamicMethod("KOE_FarCachedColliders", typeof(Collider[]), new[] { typeof(Component) }, typeof(ColliderOwners), true);
            var il = dm.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Castclass, moduleType);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ret);
            colliders = (Func<Component, Collider[]>)dm.CreateDelegate(typeof(Func<Component, Collider[]>));
            var type = AccessTools.TypeByName("ferram4.FARWingInteraction");
            var lookup = AccessTools.DeclaredMethod(type, "ExposureHitDetectionAndWingDetection");
            var wing = AccessTools.DeclaredMethod(type, "UpdateWingInteraction");
            var vessel = AccessTools.DeclaredMethod(AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.FARVesselAero"), "FixedUpdate");
            if (lookup == null
                || wing == null
                || vessel == null
                || Harmony.GetPatchInfo(lookup) != null
                || Harmony.GetPatchInfo(wing) != null)
                throw new NotSupportedException("The FAR wing lookup is missing or already has a Harmony patch.");
            harmony.Patch(lookup, null, null, new HarmonyMethod(typeof(ColliderOwners), "Lookup"));
            harmony.Patch(wing, new HarmonyMethod(typeof(ColliderOwners), "BeginWing"), null, null, new HarmonyMethod(typeof(ColliderOwners), "End"));
            harmony.Patch(vessel, new HarmonyMethod(typeof(ColliderOwners), "BeginBatch"), null, null, new HarmonyMethod(typeof(ColliderOwners), "End"));
        }

        static void BeginBatch(out Scope __state)
        {
            __state = new Scope
            {
                Previous = current,
                Created = true
            };
            current = Rent();
        }

        static void BeginWing(List<Part> __0, out Scope __state)
        {
            __state = new Scope();
            if (current == null || current.Parts != null && !ReferenceEquals(current.Parts, __0))
            {
                __state = new Scope
                {
                    Previous = current,
                    Created = true
                };
                current = Rent();
            }

            current.Parts = __0;
        }

        static Exception End(Scope __state, Exception __exception)
        {
            if (__state.Created)
            {
                current.Clear();
                pool.Push(current);
                current = __state.Previous;
            }

            return __exception;
        }

        public static IEnumerable<CodeInstruction> Lookup(IEnumerable<CodeInstruction> source, MethodBase __originalMethod)
        {
            var code = new List<CodeInstruction>(source);
            var locals = __originalMethod.GetMethodBody().LocalVariables.Where(v => v.LocalType == typeof(RaycastHit)).ToArray();
            if (locals.Length != 1)
                throw new NotSupportedException("Cannot identify the RaycastHit local used by the FAR wing lookup.");
            var enumerate = AccessTools.Method(typeof(List<Part>), "GetEnumerator");
            int edits = 0;
            for (int i = 0; i < code.Count; i++)
            {
                if (code[i].Calls(enumerate))
                {
                    if (i == 0 || code[i - 1].opcode != OpCodes.Ldarg_2 || code[i].labels.Count != 0 || code[i].blocks.Count != 0)
                        throw new NotSupportedException("The FAR part search no longer uses the expected enumeration sequence.");
                    yield return new CodeInstruction(OpCodes.Ldloc, locals[0].LocalIndex);
                    yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(ColliderOwners), "Candidates"));
                    edits++;
                }

                yield return code[i];
            }

            if (edits != 1)
                throw new NotSupportedException("Cannot find the part enumeration in the FAR wing lookup.");
        }

        public static List<Part> Candidates(List<Part> original, RaycastHit hit)
        {
            var context = current;
            if (context == null || context.Disabled || !ReferenceEquals(context.Parts, original))
                return original;
            if (!context.Built)
            {
                foreach (var part in original)
                {
                    if (part == null)
                        continue;
                    var module = part.GetComponent(moduleType);
                    Collider[] owned;
                    if (module != null)
                    {
                        owned = colliders(module);
                        // AI-generated comment: Never trigger lazy collider initialization ahead of the
                        // original lookup or change its exception/side-effect order.
                        if (owned == null)
                        {
                            context.Disabled = true;
                            return original;
                        }

                        foreach (var collider in owned)
                            Add(context, collider, part);
                    }
                    else
                        Add(context, part.collider, part);
                }

                context.Built = true;
            }

            List<Part> result;
            var target = hit.collider;
            return target != null && context.Map.TryGetValue(target, out result) ? result : empty;
        }

        static void Add(Context context, Collider collider, Part part)
        {
            if (collider == null)
                return;
            List<Part> list;
            if (!context.Map.TryGetValue(collider, out list))
            {
                list = context.Lists.Count == 0 ? new List<Part>() : context.Lists.Pop();
                context.Map.Add(collider, list);
            }

            // AI-generated comment: Keep source ordering and shared/duplicate collider ownership. The
            // original loop still excludes the parent wing and selects first match.
            list.Add(part);
        }
    }
}
