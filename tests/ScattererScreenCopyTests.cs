using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using HarmonyLib;
using KoeScattererScreenCopy;
using UnityEngine;
using UnityEngine.Rendering;

namespace Scatterer
{
    public sealed class ScreenCopyCommandBuffer : MonoBehaviour
    {
        public static readonly Dictionary<Camera, ScreenCopyCommandBuffer> CameraToCommandBufferHandler = new Dictionary<Camera, ScreenCopyCommandBuffer>();
        public bool reflectionProbeMode, isEnabled, isInitialized, hdrEnabled;
        public Camera targetCamera;
        public CommandBuffer screenCopyCommandBuffer;
        public int Builds, RenderRequests;
        [MethodImpl(MethodImplOptions.NoInlining)]
        public void Reinit()
        {
            targetCamera.RemoveCommandBuffer(CameraEvent.BeforeImageEffectsOpaque, screenCopyCommandBuffer);
            screenCopyCommandBuffer.Commands.Clear();
            if (reflectionProbeMode) screenCopyCommandBuffer.Commands.Add("GlobalTexture=black");
            else
            {
                screenCopyCommandBuffer.Commands.Add("GetTemporaryRT -1 -1 " + (hdrEnabled ? "HDR" : "ARGB32"));
                screenCopyCommandBuffer.Commands.Add("Blit CameraTarget->temporary");
                screenCopyCommandBuffer.Commands.Add("GlobalTexture=temporary");
            }
            Builds++;
        }
        public void Initialize() { screenCopyCommandBuffer = new CommandBuffer(); Reinit(); isInitialized = true; }
        public void EnableScreenCopyForFrame()
        {
            if (!isEnabled && isInitialized)
            {
                if (hdrEnabled != targetCamera.allowHDR) Reinit();
                targetCamera.Attached.Add(screenCopyCommandBuffer); isEnabled = true;
            }
        }
        public void FinishFrame() { RenderRequests++; targetCamera.Attached.Remove(screenCopyCommandBuffer); isEnabled = false; }
        public void OnDestroy() { targetCamera.RemoveCommandBuffer(CameraEvent.BeforeImageEffectsOpaque, screenCopyCommandBuffer); isEnabled = false; }
    }
}

public static class ScattererScreenCopyTests
{
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Main()
    {
        Bootstrap.Prepare(typeof(Scatterer.ScreenCopyCommandBuffer));
        var camera = new Camera { allowHDR = true };
        var reference = new Scatterer.ScreenCopyCommandBuffer { targetCamera = camera };
        reference.Initialize();
        for (int i=0;i<1000;i++) { reference.EnableScreenCopyForFrame(); reference.FinishFrame(); }
        Check(reference.Builds == 1001, "Fixture must reproduce the upstream HDR rebuild bug");
        string commands = string.Join(";", reference.screenCopyCommandBuffer.Commands);
        var harmony = new Harmony("koe.test.scatterer-screen-copy");
        harmony.Patch(AccessTools.Method(typeof(Scatterer.ScreenCopyCommandBuffer), "Reinit"),
            new HarmonyMethod(typeof(Bootstrap), "BeforeReinit"), new HarmonyMethod(typeof(Bootstrap), "AfterReinit"));
        try
        {
            var owner = new Scatterer.ScreenCopyCommandBuffer { targetCamera = camera };
            Scatterer.ScreenCopyCommandBuffer.CameraToCommandBufferHandler[camera] = owner;
            owner.Initialize();
            for (int i=0;i<1000;i++)
            {
                owner.EnableScreenCopyForFrame();
                Check(camera.Attached.Contains(owner.screenCopyCommandBuffer), "A requested frame lost its copy");
                Check(string.Join(";",owner.screenCopyCommandBuffer.Commands)==commands, "Cached command stream changed");
                owner.FinishFrame();
            }
            Check(owner.Builds == 1 && owner.RenderRequests == 1000 && Bootstrap.SkippedRebuilds == 1000,
                "Unchanged HDR camera must retain every copy while building commands once");
            camera.Width=1920;camera.Height=1080;owner.EnableScreenCopyForFrame();owner.FinishFrame();
            Check(owner.Builds==1 && owner.screenCopyCommandBuffer.Commands[0].Contains("-1 -1"), "Dynamic dimensions no longer follow the camera");
            owner.hdrEnabled=true;owner.Reinit();Check(owner.Builds==2 && owner.screenCopyCommandBuffer.Commands[0].Contains("HDR"), "Changed stored HDR format was not rebuilt");
            owner.reflectionProbeMode=true;owner.Reinit();Check(owner.Builds==3 && owner.screenCopyCommandBuffer.Commands[0]=="GlobalTexture=black", "Changed reflection mode was not rebuilt");
            owner.screenCopyCommandBuffer=new CommandBuffer();owner.Reinit();Check(owner.Builds==4, "Replacement buffer reused another buffer's cache");
            owner.isEnabled=true;owner.Reinit();Check(owner.Builds==5, "Attached command-buffer reinitialization was suppressed");owner.isEnabled=false;
            var owned = owner.screenCopyCommandBuffer;
            owner.OnDestroy();Bootstrap.AfterDestroy(owner,null);Bootstrap.AfterDestroy(owner,null);
            Check(owned.Disposals==1 && !Scatterer.ScreenCopyCommandBuffer.CameraToCommandBufferHandler.ContainsKey(camera), "Destroyed screen-copy resources retained or disposed twice");
            var replacement = new Scatterer.ScreenCopyCommandBuffer {targetCamera=camera};
            Scatterer.ScreenCopyCommandBuffer.CameraToCommandBufferHandler[camera]=replacement;
            Bootstrap.AfterDestroy(owner,null);
            Check(ReferenceEquals(Scatterer.ScreenCopyCommandBuffer.CameraToCommandBufferHandler[camera],replacement), "Old screen owner removed its replacement");
            Console.WriteLine("SCATTERER_SCREEN_COPY_PASS frames=1000 command_builds_before=1001 after=1 commands_identical=True render_requests_preserved=True invalidation_and_cleanup=True");
        }
        finally { harmony.UnpatchAll(harmony.Id); }
    }
}
