using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class VesselSilhouette
    {
        readonly Dictionary<Part, Renderer[]> parts = new Dictionary<Part, Renderer[]>();
        readonly HashSet<Part> present = new HashSet<Part>();
        readonly List<Part> removed = new List<Part>();
        readonly List<Material> materials = new List<Material>();
        readonly Dictionary<int, Material> labels = new Dictionary<int, Material>();
        sealed class Surface { public int Submesh; public Material Label; }
        sealed class SurfaceSet { public int Key, Kind; public Surface[] Entries; }
        readonly Dictionary<Renderer, SurfaceSet> surfaces = new Dictionary<Renderer, SurfaceSet>();
        struct Candidate { public Renderer Renderer; public Bounds Bounds; public Surface[] Surfaces; }
        readonly List<Candidate>[] candidates = new List<Candidate>[32];
        int snapshotFrame = -1;
        readonly Dictionary<Camera, float> traceTimes = new Dictionary<Camera, float>();
        readonly string traceRoot = System.Environment.GetEnvironmentVariable("KOE_LAYER_DATASET_ROOT");
        readonly HashSet<Renderer> drawn = new HashSet<Renderer>();
        readonly Plane[] frustum = new Plane[6];
        Renderer[] scenery;
        Renderer[] scaledRenderers;
        int scaledLayers;
        readonly HashSet<Renderer> celestial = new HashSet<Renderer>();
        int nextLabel = 1;
        int recordedFrame = -1;
        CelestialBody sceneryBody;
        RenderTexture image;
        Material white;
        System.IntPtr nativeImage;

        public RenderTexture Image { get { return image; } }
        public System.IntPtr NativeImage { get { return nativeImage; } }
        public int CelestialLayers { get { EnsureScaledRenderers();return scaledLayers; } }

        public bool Prepare(Camera camera)
        {
            if ((!HighLogic.LoadedSceneIsFlight && HighLogic.LoadedScene != GameScenes.SPACECENTER)
                || camera == null || camera.targetTexture != null || camera.orthographic
                || camera.cameraType != CameraType.Game
                || camera.cullingMask == ((1 << 5) | (1 << 25))
                || camera.pixelWidth != Screen.width || camera.pixelHeight != Screen.height)
                return false;
            if (sceneryBody != FlightGlobals.currentMainBody)
            {
                sceneryBody = FlightGlobals.currentMainBody;
                scenery = null; scaledRenderers = null; celestial.Clear(); surfaces.Clear(); snapshotFrame = -1;
            }
            if (white == null)
            {
                Shader shader = Shader.Find("Unlit/Color");
                if (shader == null || !shader.isSupported)
                    return false;
                white = new Material(shader) { hideFlags = HideFlags.HideAndDontSave, color = Color.white };
            }
            if (image == null || image.width != camera.pixelWidth || image.height != camera.pixelHeight)
            {
                if (image != null)
                {
                    image.Release();
                    Object.Destroy(image);
                }
                image = new RenderTexture(camera.pixelWidth, camera.pixelHeight, 24,
                    RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear)
                {
                    name = "KOE vessel silhouette",
                    hideFlags = HideFlags.HideAndDontSave,
                    antiAliasing = 1,
                    filterMode = FilterMode.Point
                };
                if (!image.Create())
                    return false;
                nativeImage = image.GetNativeTexturePtr();
            }
            return image.IsCreated();
        }

        public void Invalidate(Vessel vessel) { parts.Clear(); scenery = null; celestial.Clear(); surfaces.Clear(); snapshotFrame = -1; }

        public int Record(CommandBuffer commands, Camera camera, bool celestialOnly = false)
        {
            long begin = System.Diagnostics.Stopwatch.GetTimestamp();
            commands.SetRenderTarget(image);
            bool clear = recordedFrame != Time.frameCount || camera.clearFlags == CameraClearFlags.Color
                || camera.clearFlags == CameraClearFlags.Skybox;
            recordedFrame = Time.frameCount;
            commands.ClearRenderTarget(true, clear, new Color(0, 0, 0, 128f/255));
            commands.SetViewport(new Rect(0, 0, image.width, image.height));
            commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,
                GL.GetGPUProjectionMatrix(camera.projectionMatrix, true));
            GeometryUtility.CalculateFrustumPlanes(camera, frustum);
            int draws = 0;
            if(celestialOnly)draws=RecordCelestial(commands,camera);
            else
            {
                if (snapshotFrame != Time.frameCount) BuildSnapshot();
                for (int layer=0;layer<candidates.Length;layer++)
                {
                    List<Candidate> visible = candidates[layer];
                    if ((camera.cullingMask & (1 << layer)) == 0 || visible == null) continue;
                    foreach (Candidate candidate in visible)
                    {
                        if (!GeometryUtility.TestPlanesAABB(frustum, candidate.Bounds)) continue;
                        foreach (Surface surface in candidate.Surfaces)
                        {
                            commands.DrawRenderer(candidate.Renderer, surface.Label, surface.Submesh, 0);
                            draws++;
                        }
                    }
                }
            }
            commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            commands.SetViewport(camera.pixelRect);
            commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,
                GL.GetGPUProjectionMatrix(camera.projectionMatrix, camera.targetTexture != null));
            float traceTime;
            if (!string.IsNullOrEmpty(traceRoot) && (!traceTimes.TryGetValue(camera, out traceTime) || Time.realtimeSinceStartup >= traceTime))
            {
                traceTimes[camera] = Time.realtimeSinceStartup + 1;
                double milliseconds = 1000.0 * (System.Diagnostics.Stopwatch.GetTimestamp()-begin) / System.Diagnostics.Stopwatch.Frequency;
                System.IO.File.AppendAllText(System.IO.Path.Combine(traceRoot,"camera-layers.jsonl"),
                    "{\"t\":"+Time.realtimeSinceStartup.ToString("R",System.Globalization.CultureInfo.InvariantCulture)
                    +",\"camera\":\""+camera.name.Replace("\"","\\\"")+"\",\"draws\":"+draws
                    +",\"cpuMs\":"+milliseconds.ToString("R",System.Globalization.CultureInfo.InvariantCulture)+"}\n");
            }
            return draws;
        }

        int RecordCelestial(CommandBuffer commands,Camera camera)
        {
            EnsureScaledRenderers();
            int draws=0;
            foreach(Renderer renderer in scaledRenderers)
            {
                if(renderer==null || !renderer.enabled || !renderer.gameObject.activeInHierarchy
                    || (camera.cullingMask&(1<<renderer.gameObject.layer))==0
                    || !GeometryUtility.TestPlanesAABB(frustum,renderer.bounds))continue;
                foreach(Surface surface in GetSurfaces(renderer,renderer.GetInstanceID(),4))
                {commands.DrawRenderer(renderer,surface.Label,surface.Submesh,0);draws++;}
            }
            return draws;
        }

        void EnsureScaledRenderers()
        {
            if(scaledRenderers!=null)return;
            var renderers=new List<Renderer>();
            foreach(CelestialBody body in FlightGlobals.Bodies)
                if(body!=null && body.scaledBody!=null)renderers.AddRange(body.scaledBody.GetComponentsInChildren<Renderer>(true));
            scaledRenderers=renderers.ToArray();scaledLayers=0;
            foreach(Renderer renderer in scaledRenderers)
                if(renderer!=null)scaledLayers|=1<<renderer.gameObject.layer;
        }

        void BuildSnapshot()
        {
            snapshotFrame = Time.frameCount;
            foreach (List<Candidate> layer in candidates) if (layer != null) layer.Clear();
            present.Clear(); drawn.Clear();
            foreach (Vessel vessel in FlightGlobals.VesselsLoaded)
            {
                if (vessel == null || !vessel.loaded)
                    continue;
                foreach (Part part in vessel.parts)
                {
                    if (part == null)
                        continue;
                    present.Add(part);
                    Renderer[] renderers;
                    if (!parts.TryGetValue(part, out renderers))
                    {
                        renderers = part.GetComponentsInChildren<Renderer>(true);
                        parts.Add(part, renderers);
                    }
                    foreach (Renderer renderer in renderers)
                    {
                        if (renderer == null)
                            continue;
                        CollectSurface(renderer, vessel.GetInstanceID(), 1);
                    }
                    foreach (Renderer renderer in renderers) if (renderer != null) drawn.Add(renderer);
                }
            }
            removed.Clear();
            foreach (Part part in parts.Keys)
                if (!present.Contains(part)) removed.Add(part);
            foreach (Part part in removed) parts.Remove(part);
            CelestialBody body = FlightGlobals.currentMainBody;
            if (body != null && body.pqsController != null)
            {
                CollectSphere(body.pqsController, 2);
                foreach (PQS child in body.pqsController.ChildSpheres)
                    if (child != null)
                        CollectSphere(child,
                            child.GetComponent<PQSMod_OceanFX>() != null ? 3 : 2);
            }
            if (scenery == null)
            {
                foreach (CelestialBody planet in FlightGlobals.Bodies)
                    if (planet != null && planet.scaledBody != null)
                        foreach (Renderer renderer in planet.scaledBody.GetComponentsInChildren<Renderer>(true))
                            if (renderer != null) celestial.Add(renderer);
                var sceneRenderers = new List<Renderer>();
                foreach (Renderer renderer in Object.FindObjectsOfType<Renderer>())
                    if (renderer != null && (renderer is MeshRenderer || renderer is SkinnedMeshRenderer)
                        && !drawn.Contains(renderer)
                        && GetSurfaces(renderer, renderer.GetInstanceID(), celestial.Contains(renderer) ? 4 : 5).Length > 0)
                        sceneRenderers.Add(renderer);
                scenery = sceneRenderers.ToArray();
            }
            foreach (Renderer renderer in scenery)
                if (renderer != null && !drawn.Contains(renderer))
                {
                    CollectSurface(renderer, renderer.GetInstanceID(), celestial.Contains(renderer) ? 4 : 5);
                }
        }

        Material Label(int key, int kind)
        {
            Material label;
            if (labels.TryGetValue(key, out label)) return label;
            int id = nextLabel <= ushort.MaxValue ? nextLabel++ : 0;
            label = new Material(white) { color = new Color32((byte)kind, (byte)id, (byte)(id >> 8), 128),
                hideFlags = HideFlags.HideAndDontSave };
            labels.Add(key, label);
            return label;
        }

        void CollectSurface(Renderer renderer, int key, int kind)
        {
            if (!(renderer is MeshRenderer || renderer is SkinnedMeshRenderer)
                || !renderer.enabled || !renderer.gameObject.activeInHierarchy) return;
            Surface[] entries = GetSurfaces(renderer, key, kind);
            if (entries.Length == 0) return;
            int layer = renderer.gameObject.layer;
            if (candidates[layer] == null) candidates[layer] = new List<Candidate>();
            candidates[layer].Add(new Candidate { Renderer = renderer, Bounds = renderer.bounds, Surfaces = entries });
        }

        Surface[] GetSurfaces(Renderer renderer, int key, int kind)
        {
            SurfaceSet stored;
            if (!surfaces.TryGetValue(renderer, out stored) || stored.Key != key || stored.Kind != kind)
            {
                materials.Clear(); renderer.GetSharedMaterials(materials);
                var eligible = new List<Surface>();
                for (int submesh = 0; submesh < materials.Count; submesh++)
                {
                    Material original = materials[submesh];
                    if (original == null || original.shader == null) continue;
                    bool water = kind == 3 || original.shader.name.IndexOf("Ocean", System.StringComparison.OrdinalIgnoreCase) >= 0;
                    if (original.shader.name.StartsWith("Scatterer/", System.StringComparison.OrdinalIgnoreCase) && water) continue;
                    if (!water && (original.renderQueue >= (int)RenderQueue.AlphaTest
                        || original.GetTag("RenderType", false, "Opaque") != "Opaque")) continue;
                    eligible.Add(new Surface { Submesh = submesh, Label = Label(key, water ? 3 : kind) });
                }
                stored = new SurfaceSet { Key = key, Kind = kind, Entries = eligible.ToArray() };
                surfaces[renderer] = stored;
            }
            return stored.Entries;
        }

        void CollectSphere(PQS sphere, int kind)
        {
            if (sphere == null || sphere.quads == null || !sphere.isAlive) return;
            foreach (PQ quad in sphere.quads) CollectQuad(quad, kind);
        }

        void CollectQuad(PQ quad, int kind)
        {
            if (quad == null || !quad.isActive) return;
            if (quad.isSubdivided && quad.subNodes != null)
            {
                foreach (PQ child in quad.subNodes) CollectQuad(child, kind);
                return;
            }
            Renderer renderer = quad.meshRenderer;
            if (renderer == null) return;
            drawn.Add(renderer);
            CollectSurface(renderer, quad.sphereRoot.GetInstanceID(), kind);
        }

        public void Dispose()
        {
            parts.Clear();
            scenery = null; drawn.Clear(); celestial.Clear(); recordedFrame = -1;
            scaledRenderers = null;
            foreach (Material material in labels.Values) Object.Destroy(material);
            labels.Clear(); nextLabel = 1;
            surfaces.Clear();
            traceTimes.Clear();
            snapshotFrame = -1;
            foreach (List<Candidate> layer in candidates) if (layer != null) layer.Clear();
            if (image != null) { image.Release(); Object.Destroy(image); image = null; }
            if (white != null) { Object.Destroy(white); white = null; }
            nativeImage = System.IntPtr.Zero;
        }
    }
}
