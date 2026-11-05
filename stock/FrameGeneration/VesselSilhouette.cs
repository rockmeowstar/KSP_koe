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
        RenderTexture image;
        Material white;
        System.IntPtr nativeImage;

        public RenderTexture Image { get { return image; } }
        public System.IntPtr NativeImage { get { return nativeImage; } }

        public bool Prepare(Camera camera)
        {
            if (!HighLogic.LoadedSceneIsFlight || camera == null || camera.targetTexture != null
                || camera.pixelWidth != Screen.width || camera.pixelHeight != Screen.height)
                return false;
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

        public void Invalidate(Vessel vessel) { parts.Clear(); }

        public int Record(CommandBuffer commands, Camera camera)
        {
            commands.SetRenderTarget(image);
            commands.ClearRenderTarget(true, true, new Color(0, 0, 0, 1));
            commands.SetViewport(new Rect(0, 0, image.width, image.height));
            commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,
                GL.GetGPUProjectionMatrix(camera.projectionMatrix, true));
            present.Clear();
            int draws = 0;
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
                        if (renderer == null || !renderer.enabled || !renderer.gameObject.activeInHierarchy
                            || (!(renderer is MeshRenderer) && !(renderer is SkinnedMeshRenderer))
                            || (camera.cullingMask & (1 << renderer.gameObject.layer)) == 0)
                            continue;
                        materials.Clear();
                        renderer.GetSharedMaterials(materials);
                        for (int submesh = 0; submesh < materials.Count; submesh++)
                        {
                            Material material = materials[submesh];
                            // AI-generated comment: Transparent effects and alpha-cut geometry do not define a solid hull.
                            if (material == null || material.renderQueue >= (int)RenderQueue.AlphaTest
                                || material.GetTag("RenderType", false, "Opaque") != "Opaque")
                                continue;
                            commands.DrawRenderer(renderer, white, submesh, 0);
                            draws++;
                        }
                    }
                }
            }
            removed.Clear();
            foreach (Part part in parts.Keys)
                if (!present.Contains(part)) removed.Add(part);
            foreach (Part part in removed) parts.Remove(part);
            commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
            commands.SetViewport(camera.pixelRect);
            commands.SetViewProjectionMatrices(camera.worldToCameraMatrix,
                GL.GetGPUProjectionMatrix(camera.projectionMatrix, camera.targetTexture != null));
            return draws;
        }

        public void Dispose()
        {
            parts.Clear();
            if (image != null) { image.Release(); Object.Destroy(image); image = null; }
            if (white != null) { Object.Destroy(white); white = null; }
            nativeImage = System.IntPtr.Zero;
        }
    }
}
