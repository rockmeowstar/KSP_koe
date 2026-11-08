using System;
using System.Collections.Generic;
using KoeScattererShadowLifetime;
using UnityEngine.Rendering;

namespace UnityEngine
{
    public class MonoBehaviour { public int Stops; public void StopAllCoroutines() { Stops++; } public static void Destroy(object value) { } }
    public class Light
    {
        public readonly HashSet<CommandBuffer> Attached = new HashSet<CommandBuffer>();
        public void RemoveCommandBuffer(LightEvent stage, CommandBuffer value) { Attached.Remove(value); }
    }
    public class Camera
    {
        public bool allowHDR;
        public int Width = 2560, Height = 1440;
        public readonly HashSet<CommandBuffer> Attached = new HashSet<CommandBuffer>();
        public void RemoveCommandBuffer(CameraEvent stage, CommandBuffer value) { Attached.Remove(value); }
    }
    public static class Debug { public static void Log(string value) { } public static void LogError(string value) { throw new Exception(value); } }
}
namespace UnityEngine.Rendering
{
    public enum LightEvent { AfterShadowMap }
    public enum CameraEvent { BeforeImageEffectsOpaque }
    public sealed class CommandBuffer : IDisposable
    {
        public int Disposals;
        public readonly List<string> Commands = new List<string>();
        public void Dispose() { Disposals++; if (Disposals != 1) throw new Exception("Buffer disposed twice"); }
    }
}
public enum GameScenes { Flight }
public sealed class KSPAddon : Attribute
{
    public enum Startup { Instantly }
    public KSPAddon(Startup value, bool persistent) { }
}
public static class GameEvents
{
    public sealed class SceneEvent
    {
        public delegate void Callback(GameScenes scene);
        public Callback Callbacks;
        public void Add(Callback value) { Callbacks += value; }
        public void Remove(Callback value) { Callbacks -= value; }
    }
    public static readonly SceneEvent onGameSceneLoadRequested = new SceneEvent();
}
namespace KerbalOverEngineering.Options
{
    public static class Settings { public static bool Enabled(string key) { return true; } }
}
namespace Scatterer
{
    public sealed class ShadowMapCopier : UnityEngine.MonoBehaviour
    {
        public CommandBuffer textureCopyBuffer;
        public UnityEngine.Light m_Light;
        public bool commandBufferAdded;
        public static readonly Dictionary<UnityEngine.Light, ShadowMapCopier> instances = new Dictionary<UnityEngine.Light, ShadowMapCopier>();
        public int SceneCalls;
        public void RecreateForSceneChange(GameScenes scene) { SceneCalls++; }
        public void CreateTextureCopyCB() { textureCopyBuffer = new CommandBuffer(); }
        public void OnDestroy() { m_Light.RemoveCommandBuffer(LightEvent.AfterShadowMap, textureCopyBuffer); commandBufferAdded = false; }
    }
}
public static class ScattererShadowLifetimeTests
{
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Main()
    {
        Bootstrap.Prepare(typeof(Scatterer.ShadowMapCopier));
        var light = new UnityEngine.Light();
        var owner = new Scatterer.ShadowMapCopier { m_Light = light };
        Scatterer.ShadowMapCopier.instances[light] = owner;
        GameEvents.onGameSceneLoadRequested.Add(owner.RecreateForSceneChange);
        for (int i = 0; i < 100; i++)
        {
            var old = owner.textureCopyBuffer;
            Bootstrap.BeforeReplace(owner);
            if (old != null) Check(old.Disposals == 1 && !light.Attached.Contains(old), "Replaced buffer still retained");
            Check(!owner.commandBufferAdded, "Replacement leaves stale attachment state");
            owner.CreateTextureCopyCB();
            light.Attached.Add(owner.textureCopyBuffer); owner.commandBufferAdded = true;
        }
        Check(Bootstrap.ReleasedBuffers == 99 && light.Attached.Count == 1, "Repeated recreation accumulates buffers");
        var final = owner.textureCopyBuffer;
        owner.OnDestroy();
        var upstream = new InvalidOperationException("upstream sentinel");
        Check(ReferenceEquals(Bootstrap.AfterDestroy(owner, upstream), upstream), "Upstream exception was changed");
        Check(final.Disposals == 1 && light.Attached.Count == 0, "Final buffer not disposed");
        Check(!Scatterer.ShadowMapCopier.instances.ContainsKey(light), "Destroyed owner remains in registry");
        Check(GameEvents.onGameSceneLoadRequested.Callbacks == null && owner.Stops == 1, "Callback/coroutine remains active");
        Bootstrap.AfterDestroy(owner, null);
        Check(final.Disposals == 1, "Repeated cleanup double-disposes");
        var replacement = new Scatterer.ShadowMapCopier { m_Light = light };
        Scatterer.ShadowMapCopier.instances[light] = replacement;
        GameEvents.onGameSceneLoadRequested.Add(replacement.RecreateForSceneChange);
        Bootstrap.AfterDestroy(owner, null);
        Check(ReferenceEquals(Scatterer.ShadowMapCopier.instances[light], replacement), "Old owner removed its replacement");
        GameEvents.onGameSceneLoadRequested.Callbacks(GameScenes.Flight);
        Check(replacement.SceneCalls == 1 && owner.SceneCalls == 0, "Cleanup detached another owner's callback");
        Bootstrap.AfterDestroy(replacement, null);
        Console.WriteLine("SCATTERER_LIFETIME_PASS recreations=100 owned_buffers_disposed=100 registry_and_events_clean=True replacement_preserved=True exception_preserved=True");
    }
}
