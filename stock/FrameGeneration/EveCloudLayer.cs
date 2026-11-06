using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace KoeFrameGeneration
{
    sealed class EveCloudLayer
    {
        readonly Dictionary<Camera, Component> cameras = new Dictionary<Camera, Component>();
        Type rendererType;
        FieldInfo history, flip, active;
        PropertyInfo item;
        FieldInfo screen,compositeMaterial;
        bool resolved, failed;
        int frame = -1;
        UnityEngine.Texture image;
        public float Fade { get; private set; }

        public UnityEngine.Texture Image { get { return frame == Time.frameCount ? image : null; } }

        public void Observe(Camera camera)
        {
            if (camera == null || failed || camera.orthographic || camera.cameraType != CameraType.Game
                || camera.pixelWidth != Screen.width || camera.pixelHeight != Screen.height) return;
            if (frame != Time.frameCount) { frame = Time.frameCount; image = null; }
            if (!resolved)
            {
                resolved = true;
                foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if ((rendererType = assembly.GetType("Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer", false)) != null) break;
                if (rendererType == null) return;
                const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
                history = rendererType.GetField("historyRT", flags);
                flip = rendererType.GetField("useFlipUpscalingBuffer", flags);
                active = rendererType.GetField("renderingEnabled", flags);
                if (history != null) item = history.FieldType.GetProperty("Item", new[] { typeof(bool), typeof(bool), typeof(int) });
                screen=rendererType.GetField("deferredRaymarchedRendererToScreen",BindingFlags.Static|BindingFlags.NonPublic);
                if(screen!=null)compositeMaterial=screen.FieldType.GetField("compositeColorMaterial",flags);
                failed = history == null || flip == null || active == null || item == null;
            }
            if (rendererType == null || failed) return;
            Component renderer;
            if (!cameras.TryGetValue(camera, out renderer) || renderer == null)
            {
                renderer = camera.GetComponent(rendererType);
                if (renderer != null) cameras[camera]=renderer;
            }
            if (renderer == null) return;
            try
            {
                if (!(bool)active.GetValue(renderer)) return;
                object textures = history.GetValue(renderer);
                if (textures == null) return;
                // AI-generated comment: Record the buffer selected before EVE's OnPostRender
                // flips its history index and clears the public shader-global texture.
                image = item.GetValue(textures, new object[] { (bool)flip.GetValue(renderer), false, 0 }) as UnityEngine.Texture;
                var source=screen==null?null:screen.GetValue(null);
                var material=source==null||compositeMaterial==null?null:compositeMaterial.GetValue(source) as Material;
                Fade=material==null?0f:material.GetFloat("cloudFade");
            }
            catch (Exception error)
            {
                failed = true; image = null;
                Debug.LogWarning("[KoeFrameGeneration] EVE layer source unavailable: " + error.Message);
            }
        }

        public void Clear() { cameras.Clear(); image = null; frame = -1; }
    }
}
