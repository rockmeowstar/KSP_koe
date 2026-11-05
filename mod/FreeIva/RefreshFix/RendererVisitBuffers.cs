using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using HarmonyLib;
using UnityEngine;

namespace KoeFreeIvaRefresh
{
    public static class RendererVisitBuffers
    {
        [ThreadStatic]
        static List<List<Renderer>> active;
        [ThreadStatic]
        static Stack<List<Renderer>> free;
        static readonly Func<List<Renderer>, Renderer[]> items = Reader();
        static Func<List<Renderer>, Renderer[]> Reader()
        {
            var field = AccessTools.Field(typeof(List<Renderer>), "_items");
            if (field == null || field.FieldType != typeof(Renderer[]))
                throw new NotSupportedException("List<Renderer> must expose a Renderer[] backing array named _items.");
            var method = new DynamicMethod("KOE_RendererVisitArray", typeof(Renderer[]), new[] { typeof(List<Renderer>) }, typeof(RendererVisitBuffers), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ret);
            return (Func<List<Renderer>, Renderer[]>)method.CreateDelegate(typeof(Func<List<Renderer>, Renderer[]>));
        }

        public static void Install(Harmony harmony)
        {
            var type = AccessTools.TypeByName("FreeIva.InternalModuleFreeIva");
            if (type == null || type.Assembly.ManifestModule.ModuleVersionId != new Guid("44f85c42-e554-4c92-842c-629cde208456"))
                throw new NotSupportedException("Cannot find SetIsCurrentModule in the supported FreeIVA assembly.");
            var method = AccessTools.DeclaredMethod(type, "SetIsCurrentModule", new[] { typeof(bool) });
            if (method == null || Harmony.GetPatchInfo(method) != null)
                throw new NotSupportedException("Renderer visit missing/already patched");
            harmony.Patch(
                method,
                new HarmonyMethod(typeof(RendererVisitBuffers), "Begin"),
                null,
                new HarmonyMethod(typeof(RendererVisitBuffers), "Transpile"),
                new HarmonyMethod(typeof(RendererVisitBuffers), "End")
            );
        }

        static void Begin(out int __state)
        {
            if (active == null)
                active = new List<List<Renderer>>();
            __state = active.Count;
        }

        static Exception End(int __state, Exception __exception)
        {
            while (active.Count > __state)
            {
                var list = active[active.Count - 1];
                active.RemoveAt(active.Count - 1);
                list.Clear();
                free.Push(list);
            }

            return __exception;
        }

        public static Renderer[] Gather(Component component, out int count)
        {
            if (free == null)
                free = new Stack<List<Renderer>>();
            var list = free.Count == 0 ? new List<Renderer>() : free.Pop();
            active.Add(list);
            component.GetComponentsInChildren<Renderer>(false, list);
            count = list.Count;
            return items(list);
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> source, ILGenerator generator)
        {
            var count = generator.DeclareLocal(typeof(int));
            var material = generator.DeclareLocal(typeof(Material));
            int gathers = 0, lengths = 0, materials = 0;
            var getter = AccessTools.PropertyGetter(typeof(Renderer), "material");
            foreach (var instruction in source)
            {
                var method = instruction.operand as MethodInfo;
                if (method != null
                    && method.Name == "GetComponentsInChildren"
                    && method.IsGenericMethod
                    && method.ReturnType == typeof(Renderer[])
                    && method.GetParameters().Length == 0)
                {
                    var address = new CodeInstruction(OpCodes.Ldloca, count);
                    address.labels.AddRange(instruction.labels);
                    address.blocks.AddRange(instruction.blocks);
                    yield return address;
                    yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(RendererVisitBuffers), "Gather"));
                    gathers++;
                }
                else if (instruction.opcode == OpCodes.Ldlen)
                {
                    var pop = new CodeInstruction(OpCodes.Pop);
                    pop.labels.AddRange(instruction.labels);
                    pop.blocks.AddRange(instruction.blocks);
                    yield return pop;
                    yield return new CodeInstruction(OpCodes.Ldloc, count);
                    lengths++;
                }
                else if (instruction.Calls(getter))
                {
                    materials++;
                    if (materials == 1)
                    {
                        yield return instruction;
                        yield return new CodeInstruction(OpCodes.Dup);
                        yield return new CodeInstruction(OpCodes.Stloc, material);
                    }
                    else
                    {
                        var pop = new CodeInstruction(OpCodes.Pop);
                        pop.labels.AddRange(instruction.labels);
                        pop.blocks.AddRange(instruction.blocks);
                        yield return pop;
                        yield return new CodeInstruction(OpCodes.Ldloc, material);
                    }
                }
                else
                    yield return instruction;
            }

            if (gathers != 1 || lengths != 1 || materials != 2)
                throw new NotSupportedException("SetIsCurrentModule must contain one renderer gather, one length read and two material reads.");
        }
    }
}
