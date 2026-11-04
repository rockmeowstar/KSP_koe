using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Collections.Generic;
using System.Security.Cryptography;
using UnityEngine;
using UnityEngine.Rendering;

namespace KerbalOverEngineering.Options
{
    public static class Settings
    {
        static OptionStore store;
        static bool failed;
        static readonly object gate = new object();
        static readonly HashSet<string> compatibilityMessages = new HashSet<string>();
        public static string ConfigPath { get { return Path.Combine(KSPUtil.ApplicationRootPath, "GameData/KerbalOverEngineering/Options.cfg"); } }
        public static bool Enabled(string key)
        {
            lock (gate)
            {
                if (failed) return false;
                if (store == null)
                {
                    try
                    {
                        var support = new Support();
                        store = new OptionStore(ConfigPath, support.Check);
                        Debug.Log("[KOE Runtime] " + RuntimeCompatibility.Description);
                        Debug.Log("[KOE Options] " + (store.Created ? "Generated " : "Read ") + ConfigPath);
                        foreach (Feature feature in Catalog.Features)
                            Debug.Log("[KOE Options] " + feature.Key + "=" + store.Enabled(feature.Key) + (store.Reason(feature.Key) == null ? "" : "; " + store.Reason(feature.Key)));
                    }
                    catch (Exception error) { failed = true; Debug.LogError("[KOE Options] Cannot load config; fixes disabled: " + error); return false; }
                }
                if (!store.Enabled(key)) return false;
                string reason;
                try { reason = CommunityFixes.Reason(key); }
                catch (Exception error) { reason = "compatibility state unavailable: " + error.GetType().Name; }
                if (reason == null) return true;
                if (compatibilityMessages.Add(key + ":" + reason)) Debug.Log("[KOE Compatibility] " + key + " disabled: " + reason);
                return false;
            }
        }
        sealed class Support
        {
            readonly Assembly[] assemblies = AppDomain.CurrentDomain.GetAssemblies();
            readonly Dictionary<string,string> hashes = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            string FileProblem(string relative,string expected)
            {
                string path=Path.Combine(KSPUtil.ApplicationRootPath,relative);
                string hash;
                if(!hashes.TryGetValue(path,out hash))
                {
                    if(!File.Exists(path))return relative+" is missing";
                    using(var sha=SHA256.Create())using(var stream=File.OpenRead(path))hash=BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","").ToLowerInvariant();
                    hashes[path]=hash;
                }
                return hash==expected ? null : relative+" identity is unverified";
            }
            string AssemblyProblem(string name, string mvid = null)
            {
                var assembly = assemblies.FirstOrDefault(a => a.GetName().Name == name);
                if (assembly == null) return name + " is missing";
                if (mvid != null && assembly.ManifestModule.ModuleVersionId != new Guid(mvid)) return name + " version is unverified";
                return null;
            }
            string Direct3D()
            {
                if (Environment.OSVersion.Platform != PlatformID.Win32NT || IntPtr.Size != 8) return "Windows x64 required";
                if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11) return "Direct3D 11 required";
                return null;
            }
            public string Check(Feature feature)
            {
                string problem = AssemblyProblem("Assembly-CSharp", "4b449f28-41f8-4227-adfa-ad3149c8fdba");
                if (problem != null) return problem;
                problem = AssemblyProblem("0Harmony");
                if (problem != null && feature.Requirement != "D3D11" && feature.Requirement != "OpticalFlow" && feature.Requirement != "Native" && feature.Requirement != "EveD3D11") return problem;
                if(feature.Key=="Stock.PqsPatchStateMemo")
                    return AssemblyProblem("0Harmony","212c5fc0-752a-439c-8523-73c986c00088") ?? FileProblem("GameData/000_Harmony/0Harmony.dll","19ce60ac3280f72ec1751d36a40cb7e2fece2934df8345969dc7feb83bd633e4");
                switch (feature.Requirement)
                {
                    case "Native":
                        return Environment.OSVersion.Platform == PlatformID.Win32NT && IntPtr.Size == 8 ? FileProblem("UnityPlayer.dll","e661981f033e2f1d32db6e44d43c0e0c1783b4cb3115caf393e7722673473317") : "Windows x64 required";
                    case "EVE":
                    case "EveD3D11":
                        problem = AssemblyProblem("Atmosphere", "8150e218-893c-4b2c-a852-b674a3006cfd");
                        if (problem != null) return problem;
                        if (feature.Name == "TexturePrefetch" || feature.Name == "DdsDirectUpload") return AssemblyProblem("Utils", "752c32c2-9c32-40be-bad9-8a27f80a0a26");
                        return feature.Requirement == "EveD3D11" ? Direct3D() : null;
                    case "Parallax":
                        return AssemblyProblem("ParallaxContinued", "1ecb998e-ce0b-4bfd-970f-fe0fa2d99def") ??
                            (feature.Name == "BiomeRgbPreparation" ? AssemblyProblem("Kopernicus", "7fd28341-4f5a-4edf-b5f4-35706c820e0e") : null);
                    case "Firefly":
                        return AssemblyProblem("Firefly") ?? AssemblyProblem("FireflyAPI") ??
                            FileProblem("GameData/Firefly/Plugins/Firefly.dll","e312eede3f5f4837aadae302c721f8cbd67222b8722a5f01a5899f29dc288829") ??
                            FileProblem("GameData/FireflyAPI/Plugins/FireflyAPI.dll","b9fd1f72f3dd3fce34caf1ff424172af881be5e2262117fdec88268d7ebe637d") ??
                            FileProblem("GameData/Firefly/Assets/fireflybundle.ksp","abe1db2b133decab1c879ac03660dee919b3e245b6c659883a0ddd527ce6be28") ??
                            (feature.Name == "DepthLighting" ? Direct3D() : null);
                    case "D3D11": return Direct3D() ?? AssemblyProblem("Deferred");
                    case "OpticalFlow":
                        problem = Direct3D(); if (problem != null) return problem;
                        // Proton may expose a compatibility vendor ID and provide
                        // builtin DLLs without ordinary System32 files. Probe the
                        // actual adapter/API; never treat file existence as support.
                        if (!RuntimeCompatibility.IsWine && SystemInfo.graphicsDeviceVendorID != 0x10de) return "NVIDIA Optical Flow hardware required";
                        return HardwareProbe.OpticalFlow();
                }
                return null;
            }
        }
    }
}
