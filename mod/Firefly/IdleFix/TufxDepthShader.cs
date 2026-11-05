using System;
using System.IO;
using HarmonyLib;
using UnityEngine;

namespace FireflyIdleFix
{
    internal static class TufxDepthShader
    {
        static bool inspected;
        static Shader certified;
        // AI-generated comment: All three passes of this exact asset have fixed ZWrite Off and
        // stencil Keep (both faces). It cannot modify the depth-only target.
        internal static bool Contains(Shader shader)
        {
            if (!inspected)
            {
                inspected = true;
                try
                {
                    var loader = AccessTools.TypeByName("Shabby.Shabby");
                    string root = Path.Combine(KSPUtil.ApplicationRootPath, "GameData");
                    string file = Path.GetFullPath(Path.Combine(root, "TUFX/Shaders/tufx-stock.shab"));
                    if (loader == null
                        || !DepthLighting.Matches(loader.Assembly.Location, "d1cb357322afb1874528a452a6b57bc74f3cf1c64865680dd0d64fd0468854c9")
                        || !DepthLighting.Matches(file, "dbb8740c23ead2a40ae896b61c81a466e1d83058e0ccfc0f04a1e1834ae2c9ef"))
                        return false;
                    var paths = Directory.GetFiles(root, "tufx-stock.shab", SearchOption.AllDirectories);
                    if (paths.Length != 1 || !string.Equals(Path.GetFullPath(paths[0]), file, StringComparison.OrdinalIgnoreCase))
                        return false;
                    AssetBundle source = null;
                    foreach (var bundle in AssetBundle.GetAllLoadedAssetBundles())
                        if (bundle != null && bundle.name == "tufx-stock.shab")
                        {
                            if (source != null)
                                return false;
                            source = bundle;
                        }

                    if (source != null)
                        certified = source.LoadAsset<Shader>("assets/stockshaders/particleadd.shader");
                    if (certified != null && certified.name != "KSP/Particles/Additive")
                        certified = null;
                    if (certified != null)
                        Debug.Log("[FireflyDepthLighting] Certified TUFX depth-neutral shader id=" + certified.GetInstanceID());
                }
                catch (Exception error)
                {
                    certified = null;
                    Debug.LogWarning("[FireflyDepthLighting] TUFX shader proof unavailable: " + error.GetType().Name);
                }
            }

            return shader != null && shader == certified;
        }
    }
}
