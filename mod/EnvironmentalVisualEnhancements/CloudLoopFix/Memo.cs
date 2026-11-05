using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace CloudLoopFix
{
    // AI-generated comment: Invocation-local identities only: never reuse material state across cameras or frames.
    public sealed class MaterialSetupScope
    {
        sealed class IdentityComparer : IEqualityComparer<object>
        {
            public new bool Equals(object a, object b)
            {
                return ReferenceEquals(a, b);
            }

            public int GetHashCode(object value)
            {
                return RuntimeHelpers.GetHashCode(value);
            }
        }

        [ThreadStatic]
        static MaterialSetupScope current;
        [ThreadStatic]
        static MaterialSetupScope pool;
        readonly HashSet<object> materials = new HashSet<object>(new IdentityComparer());
        MaterialSetupScope parent, next;
        public static long AppliedSetups, SkippedSetups;
        public static void Begin(out MaterialSetupScope __state)
        {
            var scope = pool;
            if (scope == null)
                scope = new MaterialSetupScope();
            else
                pool = scope.next;
            scope.next = null;
            scope.parent = current;
            current = scope;
            __state = scope;
        }

        public static bool ShouldSetup(object material)
        {
            bool apply = current == null || current.materials.Add(material);
            if (apply)
                AppliedSetups++;
            else
                SkippedSetups++;
            return apply;
        }

        public static Exception End(Exception __exception, MaterialSetupScope __state)
        {
            if (__state != null)
            {
                current = __state.parent;
                __state.parent = null;
                __state.materials.Clear();
                __state.next = pool;
                pool = __state;
            }

            return __exception;
        }
    }

    // AI-generated comment: compare the float bits in each key; a matching hash alone does not identify the geometry.
    [StructLayout(LayoutKind.Explicit)]
    struct FloatBits
    {
        [FieldOffset(0)]
        public float Value;
        [FieldOffset(0)]
        public int Bits;
    }

    public struct BoundKey : IEquatable<BoundKey>
    {
        readonly int radius, nature;
        readonly object layer;
        public BoundKey(float radius, object layer, int nature)
        {
            this.radius = new FloatBits
            {
                Value = radius
            }.Bits;
            this.layer = layer;
            this.nature = nature;
        }

        public bool Equals(BoundKey other)
        {
            return radius == other.radius && nature == other.nature && ReferenceEquals(layer, other.layer);
        }
    }

    public sealed class ListMemo<T, TKey, TResult>
        where TKey : IEquatable<TKey>
    {
        TKey[] keys = new TKey[0];
        int count;
        bool valid;
        TResult value;
        public long Hits, Misses;
        public bool TryGet(List<T> source, Func<T, TKey> key, out TResult result)
        {
            if (valid && count == source.Count)
            {
                int i = 0;
                while (i < count && keys[i].Equals(key(source[i])))
                    i++;
                if (i == count)
                {
                    Hits++;
                    result = value;
                    return true;
                }
            }

            Misses++;
            result = default(TResult);
            return false;
        }

        public void Commit(List<T> source, Func<T, TKey> key, TResult result)
        {
            valid = false;
            if (keys.Length < source.Count)
                keys = new TKey[source.Count];
            for (int i = 0; i < source.Count; i++)
                keys[i] = key(source[i]);
            // AI-generated comment: Drop references to layers that have disappeared from a shrinking list.
            if (count > source.Count)
                Array.Clear(keys, source.Count, count - source.Count);
            count = source.Count;
            value = result;
            valid = true;
        }
    }

    public static class LayerOrder<T>
        where T : class
    {
        sealed class Entry
        {
            public T[] Items = new T[0];
            public float[] Keys = new float[0];
            public List<T> Sorted;
        }

        struct Rank
        {
            public float Key;
            public int Index;
        }

        sealed class RankComparer : IComparer<Rank>
        {
            public int Compare(Rank a, Rank b)
            {
                int c = a.Key.CompareTo(b.Key);
                return c != 0 ? c : a.Index.CompareTo(b.Index);
            }
        }

        static readonly IComparer<Rank> Comparer = new RankComparer();
        static readonly ConditionalWeakTable<List<T>, Entry> Entries = new ConditionalWeakTable<List<T>, Entry>();
        static readonly ConditionalWeakTable<List<T>, Entry>.CreateValueCallback Create = delegate
        {
            return new Entry();
        };
        public static long Hits, Misses;
        // AI-generated comment: The verified caller reads the returned list; it never modifies or retains it.
        public static List<T> Sort(IEnumerable<T> source, Func<T, float> selector)
        {
            var list = source as List<T>;
            if (list == null)
                throw new NotSupportedException("The cloud layer collection is not the list type used by the supported EVE build.");
            Entry e = Entries.GetValue(list, Create);
            int count = list.Count;
            var previous = e.Sorted;
            e.Sorted = null; // A throwing key getter must not leave partially updated keys attached to an old result.
            bool same = previous != null && e.Items.Length == count;
            if (e.Items.Length != count)
            {
                e.Items = new T[count];
                e.Keys = new float[count];
            }

            // AI-generated comment: One key read per element, including hits; live order edits invalidate immediately.
            for (int i = 0; i < count; i++)
            {
                T item = list[i];
                float key = selector(item);
                same &= ReferenceEquals(e.Items[i], item) && e.Keys[i].Equals(key);
                e.Items[i] = item;
                e.Keys[i] = key;
            }

            if (same)
            {
                Hits++;
                e.Sorted = previous;
                return previous;
            }

            Misses++;
            var ranks = new Rank[count];
            for (int i = 0; i < count; i++)
                ranks[i] = new Rank
                {
                    Key = e.Keys[i],
                    Index = i
                };
            Array.Sort(ranks, Comparer);
            var result = new List<T>(count);
            for (int i = 0; i < count; i++)
                result.Add(e.Items[ranks[i].Index]);
            e.Sorted = result;
            return result;
        }
    }
}
