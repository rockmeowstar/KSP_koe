using System;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;

namespace KoeFrameGeneration
{
    static class DeferredStencilProvider
    {
        public static bool RequestedAndSupported()
        {
            if(Environment.GetEnvironmentVariable("KOE_LAYER_STENCIL")!="1")return false;
            foreach(Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                if(assembly.GetType("Deferred.ShaderLoader",false)==null)continue;
                // AI-generated comment: The two low stencil bits are a provider contract.
                // Unknown shader bundles keep the renderer-based fallback until verified.
                string folder=Path.GetDirectoryName(assembly.Location);
                if(string.IsNullOrEmpty(folder))return false;
                string bundle=Path.Combine(folder,"Shaders","replacementshaders.shab");
                try
                {
                    using(var stream=File.OpenRead(bundle))
                    using(var hash=SHA256.Create())
                        return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-","").ToLowerInvariant()
                            =="410f97b61be137953bf0be439e50203af88a470742b12c2591a97cb126cb6c71";
                }
                catch(IOException){return false;}
                catch(UnauthorizedAccessException){return false;}
            }
            return false;
        }
    }
}
