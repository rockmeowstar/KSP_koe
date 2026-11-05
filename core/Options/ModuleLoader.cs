using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using UnityEngine;

namespace KerbalOverEngineering.Options
{
    public static class ModuleLoader
    {
        public static void Start(Assembly wrapper)
        {
            if (CommunityFixes.Present)
            {
                string keys;
                using (var reader = new StreamReader(wrapper.GetManifestResourceStream("module.info")))
                    keys = reader.ReadLine();
                bool postLoad = false, loader = false;
                foreach (string key in keys.Split(','))
                {
                    postLoad |= CommunityFixes.NeedsPostLoad(key.Trim());
                    loader |= CommunityFixes.ReplacesStockLoader(key.Trim());
                }

                if (postLoad || loader)
                {
                    var owner = new GameObject("KOE compatibility initialization");
                    UnityEngine.Object.DontDestroyOnLoad(owner);
                    owner.AddComponent<CompatibilityStart>().Begin(wrapper, postLoad);
                    return;
                }
            }

            StartNow(wrapper);
        }

        internal static void StartNow(Assembly wrapper)
        {
            try
            {
                string[] info;
                using (var reader = new StreamReader(wrapper.GetManifestResourceStream("module.info")))
                    info = reader.ReadToEnd().Trim().Split('\n');
                bool enabled = false;
                foreach (string key in info[0].Trim().Split(','))
                    enabled |= Settings.Enabled(key.Trim());
                if (!enabled)
                    return;
                byte[] bytes;
                using (var stream = wrapper.GetManifestResourceStream("module.bin"))
                using (var output = new MemoryStream())
                {
                    stream.CopyTo(output);
                    bytes = output.ToArray();
                }

                Assembly module = Assembly.Load(bytes);
                for (int i = 1; i < info.Length; i++)
                {
                    string name = info[i].Trim();
                    if (name.Length == 0)
                        continue;
                    // AI-generated comment: Optional dependencies have been checked before resolving any payload type.
                    Type type = module.GetType(name, true);
                    var owner = new GameObject("KOE " + name);
                    UnityEngine.Object.DontDestroyOnLoad(owner);
                    owner.AddComponent(type);
                }
            }
            catch (Exception error)
            {
                Debug.LogError("[KOE] Module disabled: " + wrapper.GetName().Name + ": " + error);
            }
        }
    }

    public sealed class CompatibilityStart : MonoBehaviour
    {
        public void Begin(Assembly wrapper, bool postLoad)
        {
            StartCoroutine(Wait(wrapper, postLoad));
        }

        IEnumerator Wait(Assembly wrapper, bool postLoad)
        {
            // AI-generated comment: All Instantly add-ons get their Awake before the next frame. KSPCF
            // applies its configured BasePatches later, in the MM post-load callback.
            yield return null;
            while (postLoad && !CommunityFixes.Ready)
            {
                if (HighLogic.LoadedScene != GameScenes.LOADING)
                {
                    Debug.LogWarning("[KOE Compatibility] " + wrapper.GetName().Name + " disabled: KSPCF patch state unavailable");
                    Destroy(gameObject);
                    yield break;
                }

                yield return null;
            }

            ModuleLoader.StartNow(wrapper);
            Destroy(gameObject);
        }
    }

    public static class NativeAssets
    {
        // AI-generated comment: Native DLLs are embedded, never scanned as managed GameData plugins.
        // Content-addressed directories keep upgrades and different modules separate.
        public static string Extract(Assembly assembly)
        {
            string folder;
            using (var sha = SHA256.Create())
            {
                byte[] identity;
                using (var stream = assembly.GetManifestResourceStream("native.manifest"))
                using (var output = new MemoryStream())
                {
                    stream.CopyTo(output);
                    identity = output.ToArray();
                }

                folder = Path.Combine(
                    KSPUtil.ApplicationRootPath,
                    "KerbalOverEngineering-cache/native/" + BitConverter.ToString(sha.ComputeHash(identity)).Replace("-", "").ToLowerInvariant()
                );
            }

            Directory.CreateDirectory(folder);
            foreach (string resource in assembly.GetManifestResourceNames())
            {
                if (!resource.StartsWith("native.file.", StringComparison.Ordinal))
                    continue;
                string name = resource.Substring("native.file.".Length);
                if (Path.GetFileName(name) != name)
                    throw new InvalidDataException("Unsafe native asset name");
                byte[] bytes;
                using (var stream = assembly.GetManifestResourceStream(resource))
                using (var output = new MemoryStream())
                {
                    stream.CopyTo(output);
                    bytes = output.ToArray();
                }

                string path = Path.Combine(folder, name);
                if (File.Exists(path) && Equal(File.ReadAllBytes(path), bytes))
                    continue;
                string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    File.WriteAllBytes(temporary, bytes);
                    if (File.Exists(path))
                        File.Replace(temporary, path, null);
                    else
                        File.Move(temporary, path);
                }
                finally
                {
                    if (File.Exists(temporary))
                        File.Delete(temporary);
                }
            }

            return folder;
        }

        static bool Equal(byte[] a, byte[] b)
        {
            if (a.Length != b.Length)
                return false;
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(a)) == BitConverter.ToString(sha.ComputeHash(b));
        }
    }
}
