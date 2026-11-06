using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class EffectLayers : IDisposable
    {
        sealed class Capture
        {
            public Camera Camera;
            public CameraEvent Event;
            public CommandBuffer Before, After;
        }
        readonly List<Capture> captures = new List<Capture>();
        readonly Dictionary<Camera, CameraEvent> stages = new Dictionary<Camera, CameraEvent>();

        public void Prepare(Camera camera, IntPtr renderEvent)
        {
            if (camera == null || renderEvent == IntPtr.Zero) return;
            CameraEvent selected;
            if (!stages.TryGetValue(camera, out selected))
            {
                selected = CameraEvent.AfterForwardAlpha;
                foreach (CameraEvent candidate in Enum.GetValues(typeof(CameraEvent)))
                    foreach (CommandBuffer buffer in camera.GetCommandBuffers(candidate))
                        if (buffer.name.StartsWith("Firefly atmospheric effects [", StringComparison.Ordinal)) selected = candidate;
                stages.Add(camera, selected);
            }
            CameraEvent stage = selected;
            {
                CommandBuffer[] buffers = camera.GetCommandBuffers(stage);
                Capture capture = captures.Find(c => c.Camera == camera && c.Event == stage);
                int first = -1, last = -1;
                for (int i = 0; i < buffers.Length; i++)
                    if (buffers[i].sizeInBytes > 0 && buffers[i].name.StartsWith("Firefly atmospheric effects [", StringComparison.Ordinal))
                    {
                        if (first < 0) first = i;
                        last = i;
                    }
                if (first < 0) { Remove(capture); return; }
                bool contiguous = true;
                for (int i = first; i <= last; i++)
                    if (buffers[i] != (capture == null ? null : capture.Before)
                        && buffers[i] != (capture == null ? null : capture.After)
                        && !buffers[i].name.StartsWith("Firefly atmospheric effects [", StringComparison.Ordinal))
                        contiguous = false;
                if (!contiguous) { Remove(capture); return; }
                if (capture != null && first > 0 && last + 1 < buffers.Length
                    && buffers[first-1] == capture.Before && buffers[last+1] == capture.After) return;
                if (capture == null)
                {
                    capture = new Capture { Camera = camera, Event = stage,
                        Before = new CommandBuffer { name = "KOE before Firefly" },
                        After = new CommandBuffer { name = "KOE after Firefly" } };
                    capture.Before.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                    capture.Before.IssuePluginEvent(renderEvent, 3);
                    capture.After.IssuePluginEvent(renderEvent, 4);
                    captures.Add(capture);
                }
                // AI-generated comment: Keep the original command buffers and their exact order.
                // The two probes copy pixels; they do not replay or replace the effect's draws.
                foreach (CommandBuffer buffer in buffers) camera.RemoveCommandBuffer(stage, buffer);
                for (int i = 0; i < buffers.Length; i++)
                {
                    if (i == first) camera.AddCommandBuffer(stage, capture.Before);
                    if (buffers[i] != capture.Before && buffers[i] != capture.After)
                        camera.AddCommandBuffer(stage, buffers[i]);
                    if (i == last) camera.AddCommandBuffer(stage, capture.After);
                }
            }
        }

        void Remove(Capture capture)
        {
            if (capture == null) return;
            if (capture.Camera != null)
            {
                capture.Camera.RemoveCommandBuffer(capture.Event, capture.Before);
                capture.Camera.RemoveCommandBuffer(capture.Event, capture.After);
            }
            capture.Before.Dispose(); capture.After.Dispose();
            captures.Remove(capture);
        }

        public void Dispose()
        {
            while (captures.Count > 0) Remove(captures[captures.Count-1]);
            stages.Clear();
        }
    }
}
