using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using UnityEngine;

namespace KerbalOverEngineering.Options
{
    static class ModVersionNotices
    {
        sealed class Reference
        {
            public readonly string Tab, Version, MetadataPath, AssemblyName;

            public Reference(string tab, string version, string metadataPath, string assemblyName = null)
            {
                Tab = tab;
                Version = version;
                MetadataPath = metadataPath;
                AssemblyName = assemblyName;
            }
        }

        // Reference versions from the development installation and the release history.
        static readonly Reference[] References =
        {
            new Reference("FAR", "0.16.2", "FerramAerospaceResearch/FAR.version"),
            new Reference("Deferred", "1.3.5", null, "Deferred"),
            new Reference("Scatterer", "0.902", null, "Scatterer"),
            new Reference("CameraTools", "1.37.1", "CameraTools/CameraTools.version"),
            new Reference("FreeIva", "0.2.20.2", "FreeIva/FreeIva.version"),
            new Reference("KSPCommunityFixes", "1.41.1", "KSPCommunityFixes/KSPCommunityFixes.version"),
            new Reference("Waterfall", "0.11.1", "Waterfall/Waterfall.version"),
            new Reference("EVE", "3.1.0", "EnvironmentalVisualEnhancements/EnvironmentalVisualEnhancements.version"),
            new Reference("ParallaxContinued", "1.1.0", "ParallaxContinued/ParallaxContinued.version"),
            new Reference("Firefly", "1.0.6", "Firefly/Firefly.version")
        };

        [Serializable]
        sealed class StringManifest
        {
            public string VERSION = null;
        }

        [Serializable]
        sealed class ObjectManifest
        {
            public VersionParts VERSION = null;
        }

        [Serializable]
        sealed class VersionParts
        {
            public int MAJOR = 0, MINOR = 0, PATCH = 0, BUILD = 0;
        }

        static Version Normalize(Version version)
        {
            return new Version(version.Major, version.Minor, Math.Max(0, version.Build), Math.Max(0, version.Revision));
        }

        static Version ReadManifest(string path)
        {
            string json = File.ReadAllText(path);
            try
            {
                var manifest = JsonUtility.FromJson<ObjectManifest>(json);
                var parts = manifest == null ? null : manifest.VERSION;
                if (parts != null && parts.MAJOR >= 0 && parts.MINOR >= 0 && parts.PATCH >= 0 && parts.BUILD >= 0
                    && (parts.MAJOR != 0 || parts.MINOR != 0 || parts.PATCH != 0 || parts.BUILD != 0))
                    return new Version(parts.MAJOR, parts.MINOR, parts.PATCH, parts.BUILD);
            }
            catch (ArgumentException) { } // Some manifests use a string for VERSION.

            try
            {
                var manifest = JsonUtility.FromJson<StringManifest>(json);
                Version parsed;
                if (manifest != null && Version.TryParse(manifest.VERSION, out parsed))
                    return Normalize(parsed);
            }
            catch (ArgumentException) { }
            return null;
        }

        static Version InstalledVersion(Reference reference, string gameData, Assembly[] assemblies)
        {
            if (reference.MetadataPath != null)
            {
                var path = Path.Combine(gameData, reference.MetadataPath);
                try
                {
                    if (File.Exists(path)) return ReadManifest(path);
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
                return null;
            }

            foreach (var assembly in assemblies)
            {
                var name = assembly.GetName();
                if (string.Equals(name.Name, reference.AssemblyName, StringComparison.OrdinalIgnoreCase))
                    return name.Version == null ? null : Normalize(name.Version);
            }
            return null;
        }

        public static Dictionary<string, string> Collect()
        {
            var notices = new Dictionary<string, string>(StringComparer.Ordinal);
            var gameData = Path.Combine(KSPUtil.ApplicationRootPath, "GameData");
            var assemblies = AppDomain.CurrentDomain.GetAssemblies();
            foreach (var reference in References)
            {
                Version installed = InstalledVersion(reference, gameData, assemblies);
                if (installed == null || installed >= Normalize(new Version(reference.Version))) continue;
                int components = installed.Revision == 0 ? 3 : 4;
                notices.Add(reference.Tab, "Installed version: " + installed.ToString(components)
                    + ". KOE optimizations were developed against version " + reference.Version
                    + ". Update to that version for the intended optimization support.");
            }
            return notices;
        }
    }
}
