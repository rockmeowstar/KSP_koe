using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using HarmonyLib;

namespace PqsNoiseFix
{
    // AI-generated comment: This pinned Harmony implementation publishes a new serialized byte[] for
    // every update under lock(state). Compare identities under the same lock;
    // deserialize only when a tracked state changes. Never modify Harmony state.
    internal sealed class HarmonyPatchState
    {
        readonly Dictionary<MethodBase, byte[]> state;
        MethodBase[] keys;
        byte[][] versions;
        public long FastChecks, FullChecks;
        HarmonyPatchState(Dictionary<MethodBase, byte[]> state)
        {
            this.state = state;
        }

        public static HarmonyPatchState Bind(string assemblyFile = null)
        {
            try
            {
                var assembly = typeof(Harmony).Assembly;
                if (assembly.ManifestModule.ModuleVersionId != new Guid("212c5fc0-752a-439c-8523-73c986c00088"))
                    return null;
                using (var sha = SHA256.Create())
                using (var file = File.OpenRead(assemblyFile ?? assembly.Location))
                    if (BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "").ToLowerInvariant() != "19ce60ac3280f72ec1751d36a40cb7e2fece2934df8345969dc7feb83bd633e4")
                        return null;
                var type = assembly.GetType("HarmonyLib.HarmonySharedState", true);
                var field = type.GetField("state", BindingFlags.Static | BindingFlags.NonPublic);
                if (field == null || field.FieldType != typeof(Dictionary<MethodBase, byte[]>))
                    return null;
                return new HarmonyPatchState((Dictionary<MethodBase, byte[]>)field.GetValue(null));
            }
            catch
            {
                return null;
            }
        }

        public bool Validate(Func<bool> fullCheck, Func<MethodBase[]> trackedMethods)
        {
            lock (state)
            {
                bool same = keys != null;
                if (same)
                    for (int i = 0; i < keys.Length; i++)
                    {
                        byte[] version;
                        state.TryGetValue(keys[i], out version);
                        if (!ReferenceEquals(version, versions[i]))
                        {
                            same = false;
                            break;
                        }
                    }

                if (same)
                {
                    FastChecks++;
                    return true;
                }

                FullChecks++;
                if (!fullCheck())
                {
                    keys = null;
                    versions = null;
                    return false;
                }

                // AI-generated comment: Full validation may discover an extra dependency, such as the
                // admitted Kopernicus transpiler. Snapshot it before unlocking.
                keys = trackedMethods().Distinct().ToArray();
                versions = new byte[keys.Length][];
                for (int i = 0; i < keys.Length; i++)
                    state.TryGetValue(keys[i], out versions[i]);
                return true;
            }
        }
    }
}
