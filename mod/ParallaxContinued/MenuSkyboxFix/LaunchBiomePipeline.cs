using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using System.Threading.Tasks;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace MenuSkyboxFix
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class LaunchBiomePipeline : MonoBehaviour
    {
        sealed class Job
        {
            public MapSO Source;
            public byte[] Indices, Palette, Hash;
            public int Width, Height;
            public BiomeExpansion Result;
            public Task Work;
        }

        static readonly FieldInfo Raw = AccessTools.Field(typeof(MapSO), "_data");
        static Job prepared;
        static bool closed;
        public static long UploadedMaps, ReplacedPixels, ReplacedRoundCalls, PaletteRoundCalls, PreparedHits, MaxWorkers, Preparations, Fallbacks;
        public static MethodInfo Bind(Assembly assembly, Assembly kopernicus)
        {
            string problem = KerbalOverEngineering.Options.ManagedContract.ParallaxProblem(assembly, "BiomeRgbPreparation");
            if (problem != null) throw new NotSupportedException(problem);
            problem = KerbalOverEngineering.Options.ManagedContract.IndexedBiomeProblem(kopernicus == null ? null
                : kopernicus.GetType("Kopernicus.Components.KopernicusCBAttributeMapSO", false));
            if (problem != null) throw new NotSupportedException(problem);
            if (Raw == null || Raw.FieldType != typeof(byte[]))
                throw new NotSupportedException("MapSO data changed");
            return AccessTools.DeclaredMethod(assembly.GetType("Parallax.ScatterManager", true), "DominantBodyLoaded", new[] { typeof(string) });
        }

        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("ParallaxContinued.BiomeRgbPreparation"))
            {
                Destroy(this);
                return;
            }

            const string id = "local.kerbaloverengineering.parallax-biome-pipeline";
            var harmony = new Harmony(id);
            try
            {
                var assemblies = AppDomain.CurrentDomain.GetAssemblies();
                var target = Bind(assemblies.Single(a => a.GetName().Name == "ParallaxContinued"), assemblies.Single(a => a.GetName().Name == "Kopernicus"));
                if (target == null)
                    throw new NotSupportedException("Parallax biome call changed");
                harmony.Patch(target, null, null, new HarmonyMethod(typeof(LaunchBiomePipeline), "Transpile"));
                DontDestroyOnLoad(gameObject);
                Debug.Log("[MenuSkyboxFix] Biome palette preparation: pure CPU workers during loading; original RGB24 upload on main thread.");
            }
            catch (Exception ex)
            {
                harmony.UnpatchAll(id);
                Debug.LogWarning("[MenuSkyboxFix] Biome pipeline unavailable: " + ex.Message);
                Destroy(this);
            }
        }

        static bool Supported(MapSO map)
        {
            return SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D11
                && map != null
                && KerbalOverEngineering.Options.ManagedContract.IndexedBiomeProblem(map.GetType()) == null;
        }

        static byte[] Palette(MapSO map)
        {
            var attributes = ((CBAttributeMapSO)map).Attributes;
            if (attributes == null || attributes.Length < 1 || attributes.Length > 256)
                throw new NotSupportedException("Biome palette size");
            var bytes = new byte[attributes.Length * 3];
            for (int i = 0; i < attributes.Length; i++)
            {
                var color = attributes[i].mapColor;
                bytes[i * 3] = (byte)Math.Round(color.r * 255f);
                bytes[i * 3 + 1] = (byte)Math.Round(color.g * 255f);
                bytes[i * 3 + 2] = (byte)Math.Round(color.b * 255f);
            }

            PaletteRoundCalls += bytes.Length;
            return bytes;
        }

        static Job Prepare(MapSO map)
        {
            var data = (byte[])Raw.GetValue(map);
            if (map.Width < 1
                || map.Height < 1
                || data.LongLength != (long)map.Width * map.Height
                || data.LongLength * 4 > KerbalOverEngineering.Options.LoadingResources.Current.BiomeBudget)
                throw new NotSupportedException("Biome preparation budget or shape");
            var job = new Job
            {
                Source = map,
                Indices = (byte[])data.Clone(),
                Palette = Palette(map),
                Width = map.Width,
                Height = map.Height
            };
            int degree = Environment.ProcessorCount;
            job.Work = Task.Factory.StartNew(() =>
            {
                // AI-generated comment: Only immutable primitive arrays and dimensions cross this boundary. No Unity calls.
                using (var sha = SHA256.Create())
                    job.Hash = sha.ComputeHash(job.Indices);
                job.Result = BiomePixels.Expand(job.Indices, job.Palette, job.Width, job.Height, degree);
                job.Indices = null;
            }, TaskCreationOptions.LongRunning);
            Preparations++;
            return job;
        }

        static bool Current(Job job, MapSO map)
        {
            if (job.Width != map.Width || job.Height != map.Height)
                return false;
            byte[] hash;
            using (var sha = SHA256.Create())
                hash = sha.ComputeHash((byte[])Raw.GetValue(map));
            return job.Hash.SequenceEqual(hash) && job.Palette.SequenceEqual(Palette(map));
        }

        public void Update()
        {
            if (HighLogic.LoadedScene == GameScenes.FLIGHT && FlightGlobals.ready)
            {
                Release();
                Destroy(this);
                return;
            }

            if (closed
                || prepared != null
                || PSystemManager.Instance == null
                || FlightGlobals.Bodies == null
                || FlightGlobals.Bodies.Count == 0)
                return;
            var home = FlightGlobals.GetHomeBody();
            if (home == null || !Supported(home.BiomeMap))
                return;
            try
            {
                prepared = Prepare(home.BiomeMap);
                Debug.Log(
                    "[MenuSkyboxFix] Preparing home biome RGB during " + HighLogic.LoadedScene
                    + "; size=" + prepared.Width
                    + "x" + prepared.Height
                    + "; logical=" + Environment.ProcessorCount
                );
            }
            catch (Exception ex)
            {
                Debug.LogWarning("[MenuSkyboxFix] Early biome preparation skipped: " + ex.Message);
                Release();
                Destroy(this);
            }
        }

        public static Texture2D Compile(MapSO map)
        {
            if (!Supported(map))
                return map.CompileToTexture();
            Texture2D texture = null;
            try
            {
                var job = prepared;
                if (job == null || !ReferenceEquals(job.Source, map))
                    job = Prepare(map);
                job.Work.Wait();
                if (!Current(job, map))
                {
                    if (ReferenceEquals(job, prepared))
                        prepared = null;
                    job = Prepare(map);
                    job.Work.Wait();
                    if (!Current(job, map))
                        throw new NotSupportedException("Biome changed during preparation");
                }

                if (ReferenceEquals(job, prepared))
                    PreparedHits++;
                texture = new Texture2D(map.Width, map.Height, TextureFormat.RGB24, false);
                texture.LoadRawTextureData(job.Result.Bytes);
                texture.Apply(false, true);
                UploadedMaps++;
                ReplacedPixels += (long)map.Width * map.Height;
                ReplacedRoundCalls += (long)map.Width * map.Height * 3;
                MaxWorkers = Math.Max(MaxWorkers, job.Result.Workers);
                return texture;
            }
            catch (Exception ex)
            {
                if (texture != null)
                    UnityEngine.Object.Destroy(texture);
                Fallbacks++;
                Debug.LogWarning("[MenuSkyboxFix] Original biome conversion: " + ex.Message);
                return map.CompileToTexture();
            }
        }

        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> input)
        {
            var code = input.Select(c => new CodeInstruction(c)).ToList();
            int count = 0;
            foreach (var c in code)
            {
                var method = c.operand as MethodInfo;
                if (method != null
                    && method.Name == "CompileToTexture"
                    && typeof(MapSO).IsAssignableFrom(method.DeclaringType)
                    && method.GetParameters().Length == 0
                    && method.ReturnType == typeof(Texture2D))
                {
                    c.opcode = OpCodes.Call;
                    c.operand = AccessTools.Method(typeof(LaunchBiomePipeline), "Compile");
                    count++;
                }
            }

            if (count != 1)
                throw new NotSupportedException("Parallax biome consumer changed");
            return code;
        }

        static void Release()
        {
            closed = true;
            prepared = null;
        }

        public void OnDestroy()
        {
            Release();
        }
    }
}
