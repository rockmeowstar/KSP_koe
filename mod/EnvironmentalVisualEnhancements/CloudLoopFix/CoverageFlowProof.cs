using System;
using System.IO;
using System.Security.Cryptography;
using System.Reflection;
using System.Collections.Generic;
using HarmonyLib;
using UnityEngine;

namespace CloudLoopFix
{
    internal static class CoverageFlowProof
    {
        static FieldInfo registry;
        // In these shipped programs CloudCoverage samples the original spherical
        // UV before _FlowMap. Flow displacement only reaches the BaseNoiseTexture
        // sampling positions, inside the existing coverage/density gate. It cannot
        // create density in a cap whose coverage texels are all exactly zero.
        // Admit the full asset pair, not a shader name or a mod version alone.
        internal static bool KnownAssets()
        {
            return SystemInfo.graphicsDeviceType==UnityEngine.Rendering.GraphicsDeviceType.Direct3D11&&
                Matches("GameData/EnvironmentalVisualEnhancements/eveshaders.bundle","8cedd16f65a487c4b5a64d8f5de1de45168b4d99d3126cc244dc2dc6b91c5685")&&
                Matches("GameData/scatterer/shaders/scatterershaders-directx","b4adb80bbf8cf70c2a9bd8f18206949370dbf567c499b9de368dd5bd0a570948")&&
                Matches("GameData/EnvironmentalVisualEnhancements/Plugins/ShaderLoader.dll","cacc96daa105068281d5c30fc722a7942073910e514979cbaaf09640100f20ff")&&
                Matches("GameData/scatterer/Scatterer.dll","27d02617cd4ff0b8b105c00b8ad9cce7251955bc26eee4bf1451bbd05390a9b0");
        }
        static bool Matches(string relative,string expected)
        {
            try
            {
                using(var sha=SHA256.Create())
                using(var file=File.OpenRead(Path.Combine(KSPUtil.ApplicationRootPath,relative)))
                    return BitConverter.ToString(sha.ComputeHash(file)).Replace("-","").ToLowerInvariant()==expected;
            }
            catch(IOException){return false;}
            catch(UnauthorizedAccessException){return false;}
        }
        internal static bool MaterialMatches(Material material)
        {
            if(material==null||material.shader==null)return false;
            var shader=material.shader;
            if(shader.name!="EVE/RaymarchCloud"&&shader.name!="Scatterer-EVE/RaymarchCloud")return false;
            // AssetBundle shaders need not be the object returned by Shader.Find.
            // EVE's actual registry is also the one Scatterer replaces in place.
            if(registry==null)
            {
                var loader=AccessTools.TypeByName("ShaderLoader.ShaderLoaderClass");
                if(loader==null)return false;
                registry=AccessTools.Field(loader,"shaderDictionary");
            }
            if(registry==null)return false;
            var shaders=registry.GetValue(null) as Dictionary<string,Shader>;
            Shader registered;
            return shaders!=null&&shaders.TryGetValue("EVE/RaymarchCloud",out registered)&&registered==shader;
        }
    }
}
