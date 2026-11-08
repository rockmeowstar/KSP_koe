using System;
using System.Collections.Generic;

namespace KerbalOverEngineering.Options
{
    public static class LiveOptions
    {
        sealed class Entry
        {
            internal bool Value;
            internal Func<bool, bool> Apply;
        }
        static readonly Dictionary<string, Entry> entries = new Dictionary<string, Entry>(StringComparer.Ordinal);

        public static void Register(string key, bool initial, Func<bool, bool> apply)
        {
            if (string.IsNullOrEmpty(key) || apply == null || entries.ContainsKey(key))
                throw new InvalidOperationException("Invalid live option registration: " + key);
            entries.Add(key, new Entry { Value = initial, Apply = apply });
        }

        public static void Unregister(string key) { entries.Remove(key); }
        public static bool IsAvailable(string key) { return entries.ContainsKey(key); }
        public static bool TryGet(string key, out bool value)
        {
            Entry entry;
            if (entries.TryGetValue(key, out entry)) { value = entry.Value; return true; }
            value = false;
            return false;
        }

        public static bool Apply(string key, bool value)
        {
            Entry entry;
            if (!entries.TryGetValue(key, out entry)) return false;
            if (entry.Value == value) return true;
            try
            {
                if (!entry.Apply(value)) return false;
                entry.Value = value;
                return true;
            }
            catch (Exception) { return false; }
        }
    }
}
