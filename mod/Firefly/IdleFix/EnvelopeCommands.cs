using System;
using System.Collections.Generic;
using System.Reflection;
using Firefly;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace FireflyIdleFix
{
    // AI-generated comment: Keep every original renderer, draw order, shader and HDR value. Only
    // duplicate global-state commands between adjacent draws are eliminated.
    internal static class EnvelopeCommands
    {
        static readonly string[] colorFields =
        {
            "glow",
            "glow_hot",
            "trail_primary",
            "trail_secondary",
            "trail_tertiary",
            "trail_streak",
            "wrap_layer",
            "wrap_streak",
            "shockwave"
        };
        static readonly string[] colorProperties =
        {
            "_GlowColor",
            "_HotGlowColor",
            "_PrimaryColor",
            "_SecondaryColor",
            "_TertiaryColor",
            "_StreakColor",
            "_LayerColor",
            "_LayerStreakColor",
            "_ShockwaveColor"
        };
        static readonly int modelId = Shader.PropertyToID("_ModelScale"), envelopeId = Shader.PropertyToID("_EnvelopeScaleFactor"), randomId = Shader.PropertyToID("_RandomnessFactor");
        static readonly int[] colorIds = Array.ConvertAll(colorProperties, Shader.PropertyToID);
        static MethodInfo currentConfig;
        static bool failed;
        static long originalCommands, emittedCommands, totalDraws;
        sealed class DrawState
        {
            public Renderer Renderer;
            public Vector4 Model, Envelope, Random;
            public Color[] Colors;
        }

        public static void Bind(Harmony harmony)
        {
            if (Environment.GetEnvironmentVariable("KOE_FIREFLY_GLOBALS_DISABLE") == "1")
                return;
            currentConfig = AccessTools.Method(typeof(AtmoFxModule), "GetCurrentConfig");
            if (currentConfig == null || currentConfig.ReturnType != typeof(BodyConfig))
                throw new MissingMemberException("Firefly.GetCurrentConfig");
            harmony.Patch(AccessTools.Method(typeof(AtmoFxModule), "PopulateCommandBuffer"), prefix: new HarmonyMethod(typeof(EnvelopeCommands), "Populate"));
        }

        static void Vector(CommandBuffer buffer, GlobalCommandCache cache, int id, Vector4 value, ref int emitted)
        {
            if (cache.ShouldEmit(id, GlobalValue.From(value.x, value.y, value.z, value.w)))
            {
                buffer.SetGlobalVector(id, value);
                emitted++;
            }
        }

        static bool Populate(AtmoFxModule __instance)
        {
            if (failed)
                return true;
            try
            {
                var fx = __instance.fxVessel;
                if (fx == null
                    || fx.commandBuffer == null
                    || fx.material == null
                    || fx.material.shader == null
                    || fx.material.shader.name != "Firefly/Firefly")
                    return true;
                var config = currentConfig.Invoke(__instance, null) as BodyConfig;
                if (config == null || config.colors == null)
                    return true;
                var states = new List<DrawState>(fx.fxEnvelope.Count);
                foreach (var envelope in fx.fxEnvelope)
                {
                    if (envelope.renderer == null)
                        return true;
                    BodyColors overrides;
                    ConfigManager.Instance.partConfigs.TryGetValue(envelope.partName, out overrides);
                    var colors = new Color[colorFields.Length];
                    for (int i = 0; i < colors.Length; i++)
                    {
                        HDRColor value = null;
                        if (overrides != null)
                            overrides.fields.TryGetValue(colorFields[i], out value);
                        if (value == null)
                            value = config.colors[colorFields[i]];
                        if (value == null)
                            return true;
                        colors[i] = (Color)value;
                    }

                    float randomness = envelope.partName == "PotatoRoid" || envelope.partName == "PotatoComet" ? 1f : 0f;
                    states.Add(
                        new DrawState {
                        Renderer = envelope.renderer,
                        Model = (Vector4)envelope.modelScale,
                        Envelope = (Vector4)envelope.envelopeScaleFactor,
                        Random = new Vector4(randomness, randomness, 0, 0),
                        Colors = colors
                    }
                    );
                }

                // AI-generated comment: Resolve everything before touching the attached command buffer.
                var buffer = fx.commandBuffer;
                var cache = new GlobalCommandCache();
                int emitted = 0;
                buffer.Clear();
                foreach (var state in states)
                {
                    Vector(buffer, cache, modelId, state.Model, ref emitted);
                    Vector(buffer, cache, envelopeId, state.Envelope, ref emitted);
                    Vector(buffer, cache, randomId, state.Random, ref emitted);
                    for (int i = 0; i < state.Colors.Length; i++)
                    {
                        Color c = state.Colors[i];
                        int id = colorIds[i];
                        if (cache.ShouldEmit(id, GlobalValue.From(c.r, c.g, c.b, c.a)))
                        {
                            buffer.SetGlobalColor(id, c);
                            emitted++;
                        }
                    }

                    buffer.DrawRenderer(state.Renderer, fx.material);
                }

                originalCommands += states.Count * 12;
                emittedCommands += emitted;
                totalDraws += states.Count;
                Debug.Log(
                    "[FireflyEnvelopeCommands] draws=" + states.Count
                    + "; globals=" + (states.Count * 12)
                    + "->" + emitted
                    + "; totalDraws=" + totalDraws
                    + "; totalGlobals=" + originalCommands
                    + "->" + emittedCommands
                    + "; original shaders/HDR/draw order retained"
                );
                return false;
            }
            catch (Exception ex)
            {
                failed = true;
                Debug.LogError("[FireflyEnvelopeCommands] Original path: " + ex);
                return true;
            }
        }
    }
}
