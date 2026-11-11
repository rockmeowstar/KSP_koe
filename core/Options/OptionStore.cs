using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace KerbalOverEngineering.Options
{
    public sealed class Feature
    {
        public readonly string Group, Name, Requirement, Parent;
        public readonly bool DefaultEnabled;
        public string Key
        {
            get
            {
                return Group + "." + Name;
            }
        }

        public Feature(string group, string name, string requirement, string parent, bool defaultEnabled = true)
        {
            Group = group;
            Name = name;
            Requirement = requirement;
            Parent = parent;
            DefaultEnabled = defaultEnabled;
        }
    }

    public static class Catalog
    {
        public const int SchemaVersion = 1;
        static Feature F(string group, string name, string requirement, string parent = null, bool defaultEnabled = true)
        {
            return new Feature(group, name, requirement, parent, defaultEnabled);
        }

        public static readonly Feature[] Features =
        {
            F("Stock", "ExhaustSync", "Harmony"),
            F("Stock", "PropellantMessageCache", "Harmony"),
            F("Stock", "PropellantArgumentReuse", "Harmony", "Stock.PropellantMessageCache"),
            F("Stock", "DeltaVLookup", "Harmony"),
            F("Stock", "DeltaVReadBatch", "Harmony"),
            F("Stock", "SimulatedTransfer", "Harmony"),
            F("Stock", "ExpansionBufferReuse", "Harmony"),
            F("Stock", "DdsDirectUpload", "Harmony"),
            F("Stock", "DdsPrefetch", "Harmony"),
            F("Stock", "CraftLinkIndex", "Harmony"),
            F("Stock", "DlcLz4Cache", "Harmony"),
            F("Stock", "DlcEarlyPreparation", "Harmony", "Stock.DlcLz4Cache"),
            F("Stock", "PngPipeline", "Harmony"),
            F("Stock", "ReadySceneReveal", "Harmony"),
            F("Stock", "OverlayCameraCleanup", "Harmony"),
            F("Stock", "FlightUiLifecycle", "Harmony"),
            F("Stock", "PlanetLaunchReadiness", "Harmony"),
            F("Stock", "DatabaseChildWaitDrain", "Harmony"),
            F("Stock", "DatabaseReadyAsyncDrain", "Harmony"),
            F("Stock", "DatabaseFrameBudget", "Harmony"),
            F("Stock", "NativeTextureEncoder", "Native"),
            F("Stock", "PqsNoisePreparation", "Harmony"),
            F("Stock", "PqsPatchStateMemo", "Harmony", "Stock.PqsNoisePreparation"),
            F("Stock", "PortraitFullRate", "Harmony"),
            F("Stock", "PortraitHiddenCull", "Harmony"),
            F("Stock", "StageListReuse", "Harmony"),
            F("Stock", "DeltaVResetIndex", "Harmony"),
            F("Stock", "RealtimePhysicsCatchup", "Harmony"),
            F("Stock", "PhysicsWarpPrecision", "Harmony", "Stock.RealtimePhysicsCatchup"),
            F("Stock", "EvaTriggerListReuse", "Harmony"),
            F("Stock", "PropellantGaugeLookup", "Harmony"),
            F("FAR", "RendererInitDispatch", "FAR"),
            F("FAR", "HitSortBuffers", "FAR"),
            F("FAR", "FairingLifecycle", "FAR"),
            F("FAR", "ColliderOwnerIndex", "FAR"),
            F("FAR", "RustRendererFilter", "FarNative"),
            F("FAR", "RustRendererSetup", "FarNative"),
            F("FAR", "ExposurePoolReset", "FAR"),
            F("Deferred", "SsrResourceCleanup", "Deferred"),
            F("Scatterer", "ShadowCopyLifetime", "Scatterer"),
            F("Scatterer", "ScreenCopyReuse", "Scatterer"),
            F("CameraTools", "ToolbarLifetime", "CameraTools"),
            F("FreeIva", "HatchConnectionDedup", "FreeIva"),
            F("FreeIva", "ForeignVesselRefresh", "FreeIva"),
            F("FreeIva", "RendererVisitBuffers", "FreeIva"),
            F("KSPCommunityFixes", "CollisionStateDedup", "KSPCF"),
            F("KSPCommunityFixes", "DragGpuReduction", "KspCfCompute"),
            F("KSPCommunityFixes", "EarlyDlcCache", "KSPCF"),
            F("Waterfall", "RendererMembershipIndex", "Waterfall"),
            F("EVE", "IntervalGeometryMemo", "EVE"),
            F("EVE", "LayerOrderMemo", "EVE"),
            F("EVE", "MaterialSetupDedup", "EVE"),
            F("EVE", "TexturePrefetch", "EVE"),
            F("EVE", "DdsDirectUpload", "EVE"),
            F("EVE", "MenuInitScheduler", "EVE"),
            F("EVE", "CurveBakeHoist", "EVE"),
            F("EVE", "CoverageZeroCull", "EVE"),
            F("EVE", "ShaderCorrectionGuards", "EveD3D11"),
            F("EVE", "CloudShaderPacking", "EveD3D11"),
            F("EVE", "CloudPassFusion", "EveD3D11", "EVE.CloudShaderPacking"),
            F("EVE", "CloudMapPacking", "EveD3D11", null, false),
            F("ParallaxContinued", "GpuSkyboxCopy", "Parallax"),
            F("ParallaxContinued", "DdsDirectUpload", "Parallax"),
            F("ParallaxContinued", "BiomeRgbPreparation", "Parallax"),
            F("ParallaxContinued", "LaunchTextureRetention", "Parallax"),
            F("Firefly", "ZeroStrengthGate", "Firefly"),
            F("Firefly", "DepthLighting", "Firefly"),
            F("Firefly", "GlobalCommandDedup", "Firefly"),
            F("Rendering", "InstancedPointLights", "D3D11"),
            F("Rendering", "BlendFactorNormalization", "D3D11", "Rendering.InstancedPointLights"),
            F("Rendering", "ShadowDrawBatching", "D3D11"),
            F("Rendering", "ShaderConstantUpdates", "UnityD3D11"),
            F("Rendering", "RustShaderSubmission", "UnityD3D11"),
            F("Rendering", "FrameGeneration", "OpticalFlow", defaultEnabled: false),
            F("Rendering", "AdaptiveFrameCadence", "OpticalFlow", "Rendering.FrameGeneration")
        };
    }

    // AI-generated comment: Pure filesystem/config logic. No dependency code is loaded by this class.
    public sealed class OptionStore
    {
        readonly Dictionary<string, bool> requested = new Dictionary<string, bool>(StringComparer.Ordinal);
        readonly Dictionary<string, string> unavailable = new Dictionary<string, string>(StringComparer.Ordinal);
        readonly Dictionary<string, Feature> features = new Dictionary<string, Feature>(StringComparer.Ordinal);
        readonly Func<Feature, string> defaultDisabledReason;
        public readonly string Path;
        public bool Created { get; private set; }

        // support returns null for a supported feature, otherwise a short reason.
        public OptionStore(string path, Func<Feature, string> support, Func<Feature, string> defaultDisabledReason = null)
        {
            this.defaultDisabledReason = defaultDisabledReason;
            Path = System.IO.Path.GetFullPath(path);
            foreach (Feature feature in Catalog.Features)
            {
                features.Add(feature.Key, feature);
                requested.Add(feature.Key, false); // Missing/invalid keys fail closed, including future keys.
                string reason = support(feature);
                if (reason != null)
                    unavailable.Add(feature.Key, reason);
            }

            if (!File.Exists(Path))
                CreateMissing();
            ReadExisting();
        }

        void CreateMissing()
        {
            var text = new StringBuilder();
            text.AppendLine("// KOE optional fixes. true/false; restart the game after editing.");
            text.AppendLine("// Generated only when absent. Delete this file to detect dependencies again.");
            text.AppendLine("// Existing files are never rewritten. Unknown/missing/invalid options are disabled.");
            text.AppendLine("schemaVersion = " + Catalog.SchemaVersion);
            string group = null;
            foreach (Feature feature in Catalog.Features)
            {
                if (group != feature.Group)
                {
                    group = feature.Group;
                    text.AppendLine();
                    text.AppendLine("[" + group + "]");
                }

                string reason;
                bool supported = !unavailable.TryGetValue(feature.Key, out reason);
                string upstream = defaultDisabledReason == null ? null : defaultDisabledReason(feature);
                bool enabled = supported && feature.DefaultEnabled && upstream == null;
                text.AppendLine("// Requires: " + feature.Requirement + (feature.Parent == null ? "" : "; " + feature.Parent));
                if (!supported)
                    text.AppendLine("// Unavailable: " + reason.Replace('\r', ' ').Replace('\n', ' '));
                else if (upstream != null)
                    text.AppendLine("// Default off: " + upstream.Replace('\r', ' ').Replace('\n', ' '));
                else if (!feature.DefaultEnabled)
                    text.AppendLine("// Experimental: disabled by default; enable explicitly for testing.");
                text.AppendLine(feature.Name + " = " + (enabled ? "true" : "false"));
            }

            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
            // AI-generated comment: Publish a complete file atomically. A competing creator or a manual file always wins.
            string temporary = Path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllText(temporary, text.ToString(), new UTF8Encoding(false));
                try
                {
                    File.Move(temporary, Path);
                    Created = true;
                }
                catch (IOException)
                {
                    if (!File.Exists(Path))
                        throw;
                }
            }
            finally
            {
                if (File.Exists(temporary))
                    File.Delete(temporary);
            }
        }

        void ReadExisting()
        {
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            var duplicate = new HashSet<string>(StringComparer.Ordinal);
            string group = "";
            foreach (string source in File.ReadAllLines(Path, Encoding.UTF8))
            {
                string line = source.Trim();
                if (line.Length == 0 || line.StartsWith("//") || line.StartsWith("#"))
                    continue;
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    group = line.Substring(1, line.Length - 2).Trim();
                    continue;
                }

                int split = line.IndexOf('=');
                if (split < 1)
                    continue;
                string name = line.Substring(0, split).Trim();
                string key = name == "schemaVersion" ? name : group + "." + name;
                if (values.ContainsKey(key))
                    duplicate.Add(key);
                values[key] = line.Substring(split + 1).Trim();
            }

            string schema;
            if (!values.TryGetValue("schemaVersion", out schema)
                || schema != Catalog.SchemaVersion.ToString()
                || duplicate.Contains("schemaVersion"))
                return;
            foreach (Feature feature in Catalog.Features)
            {
                string value;
                bool enabled;
                if (!duplicate.Contains(feature.Key) && values.TryGetValue(feature.Key, out value) && bool.TryParse(value, out enabled))
                    requested[feature.Key] = enabled;
            }
        }

        public bool Enabled(string key)
        {
            bool enabled;
            Feature feature;
            if (!requested.TryGetValue(key, out enabled)
                || !enabled
                || unavailable.ContainsKey(key)
                || !features.TryGetValue(key, out feature))
                return false;
            return feature.Parent == null || Enabled(feature.Parent);
        }

        public string Reason(string key)
        {
            string reason;
            if (unavailable.TryGetValue(key, out reason))
                return reason;
            Feature feature;
            if (!features.TryGetValue(key, out feature))
                return "Unknown option";
            if (!requested[key])
                return "Disabled or missing in config";
            if (feature.Parent != null && !Enabled(feature.Parent))
                return "Requires " + feature.Parent;
            return null;
        }
    }
}
