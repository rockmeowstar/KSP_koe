using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

namespace KoeDeltaVReadBatchFix
{
    // AI-generated comment: a batch lasts only until the next solver phase. It never stores live engine fuel.
    internal sealed class ReadBatch : IDisposable
    {
        struct Key : IEquatable<Key>
        {
            internal PartSet Network;
            internal int ResourceId;
            internal bool Pulling;
            public bool Equals(Key other)
            {
                return ReferenceEquals(Network, other.Network) && ResourceId == other.ResourceId && Pulling == other.Pulling;
            }

            public override bool Equals(object value)
            {
                return value is Key && Equals((Key)value);
            }

            public override int GetHashCode()
            {
                return RuntimeHelpers.GetHashCode(Network) * 397 ^ ResourceId * 2 ^ (Pulling ? 1 : 0);
            }
        }

        struct Totals
        {
            internal double Amount;
            internal double Capacity;
            internal PartSet.ResourcePrioritySet ResourceList;
        }

        static readonly Func<PartSet, Dictionary<int, PartSet.ResourcePrioritySet>> PullLists = CreateReader<PartSet, Dictionary<int, PartSet.ResourcePrioritySet>>("pullList");
        static readonly Func<PartSet, Dictionary<int, PartSet.ResourcePrioritySet>> PushLists = CreateReader<PartSet, Dictionary<int, PartSet.ResourcePrioritySet>>("pushList");
        static readonly Action<PartSet, bool> SetSimulation = CreateSimulationWriter();
        [ThreadStatic]
        static ReadBatch current;
        internal static ReadBatch Current
        {
            get
            {
                return current;
            }
        }

        readonly Dictionary<Key, Totals> totals = new Dictionary<Key, Totals>();
        readonly ReadBatch parent;
        bool disposed;
        int suppressionDepth;
        internal ReadBatch()
        {
            parent = current;
            if (parent != null)
                parent.Invalidate();
            current = this;
        }

        static Func<T, TValue> CreateReader<T, TValue>(string name)
        {
            var field = typeof(T).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (field == null || field.FieldType != typeof(TValue))
                throw new MissingFieldException(typeof(T).Name + "." + name + " must have type " + typeof(TValue).Name);
            var method = new DynamicMethod("KOE_Read_" + name, typeof(TValue), new[] { typeof(T) }, typeof(ReadBatch), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldfld, field);
            il.Emit(OpCodes.Ret);
            return (Func<T, TValue>)method.CreateDelegate(typeof(Func<T, TValue>));
        }

        static Action<PartSet, bool> CreateSimulationWriter()
        {
            var field = typeof(PartSet).GetField("simulationSet", BindingFlags.Instance | BindingFlags.NonPublic);
            if (field == null || field.FieldType != typeof(bool))
                throw new MissingFieldException("PartSet.simulationSet must be a Boolean field.");
            var method = new DynamicMethod("KOE_SetSimulation", typeof(void), new[] { typeof(PartSet), typeof(bool) }, typeof(ReadBatch), true);
            var il = method.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(OpCodes.Ldarg_1);
            il.Emit(OpCodes.Stfld, field);
            il.Emit(OpCodes.Ret);
            return (Action<PartSet, bool>)method.CreateDelegate(typeof(Action<PartSet, bool>));
        }

        internal void Invalidate()
        {
            totals.Clear();
        }

        internal static void Barrier()
        {
            if (current != null)
                current.Invalidate();
        }

        internal void Suppress()
        {
            Invalidate();
            suppressionDepth++;
        }

        internal void Resume()
        {
            if (suppressionDepth <= 0)
                throw new InvalidOperationException("Cannot resume delta-V batching without a matching suppression.");
            suppressionDepth--;
            Invalidate();
        }

        public void Dispose()
        {
            if (disposed)
                return;
            if (!ReferenceEquals(current, this))
                throw new InvalidOperationException("A nested delta-V calculation must finish before its parent.");
            Invalidate();
            current = parent;
            disposed = true;
            if (parent != null)
                parent.Invalidate();
        }

        internal static bool TryRead(PartSet network, int resourceId, bool pulling, bool simulate, out double amount, out double capacity)
        {
            amount = capacity = 0;
            var batch = current;
            if (batch == null)
                return false;
            if (batch.suppressionDepth != 0 || !simulate || ReferenceEquals(network, null) || network.GetType() != typeof(PartSet))
            {
                batch.Invalidate();
                return false;
            }

            var lists = pulling ? PullLists(network) : PushLists(network);
            PartSet.ResourcePrioritySet resourceList;
            // AI-generated comment: leave lazy list creation to KSP; it can initialize other resource state.
            if (lists == null || !lists.TryGetValue(resourceId, out resourceList) || resourceList == null)
            {
                batch.Invalidate();
                return false;
            }

            var key = new Key
            {
                Network = network,
                ResourceId = resourceId,
                Pulling = pulling
            };
            Totals value;
            if (batch.totals.TryGetValue(key, out value) && ReferenceEquals(value.ResourceList, resourceList))
            {
                SetSimulation(network, true);
                amount = value.Amount;
                capacity = value.Capacity;
                return true;
            }

            // AI-generated comment: KSP performs the sum so the order of floating-point additions stays the same.
            network.GetConnectedResourceTotals(resourceId, out amount, out capacity, pulling, true);
            batch.totals[key] = new Totals
            {
                Amount = amount,
                Capacity = capacity,
                ResourceList = resourceList
            };
            return true;
        }
    }
}
