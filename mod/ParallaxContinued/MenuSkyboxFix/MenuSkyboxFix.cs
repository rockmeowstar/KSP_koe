using System;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

[assembly: AssemblyTitle("KerbalOverEngineering Parallax Scene Loading Fix")]
[assembly: AssemblyVersion("1.1.0.0")]
namespace MenuSkyboxFix
{
    public struct FaceInfo
    {
        public int Width, Height, Mips;
        public TextureFormat Format;
        public GraphicsFormat Graphics;
    }

    public static class FacePolicy
    {
        public static bool Compatible(FaceInfo[] faces)
        {
            if (faces == null || faces.Length != 6)
                return false;
            var first = faces[0];
            if (first.Width <= 0 || (first.Width & (first.Width - 1)) != 0 || first.Height != first.Width || first.Mips <= 0)
                return false;
            int maximumMips = 1;
            for (int size = first.Width; size > 1; size >>= 1)
                maximumMips++;
            if (first.Mips > maximumMips)
                return false;
            if (first.Format != TextureFormat.BC7 && first.Format != TextureFormat.DXT1 && first.Format != TextureFormat.DXT5)
                return false;
            return faces.All(
                f => f.Width == first.Width
                && f.Height == first.Height
                && f.Mips == first.Mips
                && f.Format == first.Format
                && f.Graphics == first.Graphics
            );
        }
    }

    public static class Patch
    {
        static Harmony harmony;
        static FieldInfo cubeField, generatedField;
        static readonly string[] Names =
        {
            "XP",
            "XN",
            "YP",
            "YN",
            "ZP",
            "ZN"
        };
        public static long FastCopies, Fallbacks, CopiedFaces, AvoidedReadbacks, AvoidedCompressions;
        public static void Apply()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("ParallaxContinued.GpuSkyboxCopy"))
                return;
            if (harmony != null)
                return;
            if (typeof(ModuleEngines).Assembly.ManifestModule.ModuleVersionId != new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))
                throw new NotSupportedException("This patch requires the KSP 1.12.5 Assembly-CSharp build.");
            var assembly = AppDomain.CurrentDomain.GetAssemblies().Single(a => a.GetName().Name == "ParallaxContinued");
            if (assembly.ManifestModule.ModuleVersionId != new Guid("1ecb998e-ce0b-4bfd-970f-fe0fa2d99def"))
                throw new NotSupportedException("The Parallax assembly does not match the build this patch supports.");
            var type = assembly.GetType("Parallax.Scaled_System.SkyboxControl", true);
            cubeField = AccessTools.Field(type, "cubeMap");
            generatedField = AccessTools.Field(type, "alreadyGenerated");
            var target = AccessTools.DeclaredMethod(type, "ExtractSkyboxFrom", new[] { typeof(GameObject) });
            if (target == null
                || !target.IsStatic
                || target.ReturnType != typeof(void)
                || cubeField == null
                || cubeField.FieldType != typeof(Cubemap)
                || generatedField == null
                || !generatedField.IsStatic
                || generatedField.FieldType != typeof(bool))
                throw new NotSupportedException("Parallax skybox API changed");
            var patcher = new Harmony("local.kerbaloverengineering.parallax-menu-skybox");
            patcher.Patch(target, new HarmonyMethod(typeof(Patch), "Prefix") { priority = Priority.Last });
            harmony = patcher;
            Debug.Log(
                "[MenuSkyboxFix] Loaded 1.1.0: compatible menu/flight/tracking faces copied entirely on GPU; Parallax generation guard and stock fallback retained."
            );
        }

        public static bool Prefix(GameObject skybox)
        {
            var scene = HighLogic.LoadedScene;
            if (scene != GameScenes.MAINMENU && scene != GameScenes.FLIGHT && scene != GameScenes.TRACKSTATION)
                return true;
            // AI-generated comment: Retain Parallax's process-wide guard outside the menu.
            if (scene != GameScenes.MAINMENU && (bool)generatedField.GetValue(null))
                return false;
            Cubemap cube = null;
            int quality = QualitySettings.masterTextureLimit;
            try
            {
                if (skybox == null
                    || SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11
                    || (SystemInfo.copyTextureSupport & CopyTextureSupport.DifferentTypes) == 0)
                {
                    Fallbacks++;
                    return true;
                }

                var renderers = skybox.GetComponentsInChildren<Renderer>(true);
                if (renderers.Length != 6 || Names.Any(n => renderers.Count(r => r.name == n) != 1))
                {
                    Fallbacks++;
                    return true;
                }

                var sources = Names.Select(n => renderers.Single(r => r.name == n).material.mainTexture as Texture2D).ToArray();
                if (sources.Any(t => t == null))
                {
                    Fallbacks++;
                    return true;
                }

                if (GameSettings.TEXTURE_QUALITY != 0)
                    QualitySettings.masterTextureLimit = 0;
                var faces = sources.Select(t => new FaceInfo { Width = t.width, Height = t.height, Mips = t.mipmapCount, Format = t.format, Graphics = t.graphicsFormat })
                    .ToArray();
                if (!FacePolicy.Compatible(faces))
                {
                    Fallbacks++;
                    return true;
                }

                cube = new Cubemap(faces[0].Width, faces[0].Format, faces[0].Mips);
                // AI-generated comment: Keep the source's colour interpretation; never reinterpret sRGB bytes as linear.
                if (cube.graphicsFormat != faces[0].Graphics)
                {
                    UnityEngine.Object.Destroy(cube);
                    Fallbacks++;
                    return true;
                }

                for (int face = 0; face < 6; face++)
                    Graphics.CopyTexture(sources[face], 0, cube, face);
                UnityEngine.Object.DontDestroyOnLoad(cube);
                cubeField.SetValue(null, cube);
                if (scene != GameScenes.MAINMENU)
                    generatedField.SetValue(null, true);
                FastCopies++;
                CopiedFaces += 6;
                AvoidedReadbacks += 6;
                AvoidedCompressions += 6;
                Debug.Log("[MenuSkyboxFix] GPU copy: faces=6; size=" + faces[0].Width
                    + "; format=" + faces[0].Format
                    + "; mips=" + faces[0].Mips);
                return false;
            }
            catch (Exception ex)
            {
                if (cube != null)
                    UnityEngine.Object.Destroy(cube);
                Fallbacks++;
                Debug.LogWarning("[MenuSkyboxFix] Using stock extraction: " + ex.Message);
                return true;
            }
            finally
            {
                if (QualitySettings.masterTextureLimit != quality)
                    QualitySettings.masterTextureLimit = quality;
            }
        }
    }

    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        public void Awake()
        {
            try
            {
                Patch.Apply();
            }
            catch (Exception ex)
            {
                Debug.LogError("[MenuSkyboxFix] Disabled: " + ex);
            }

            Destroy(this);
        }
    }
}
