using System;
using System.IO;
using System.Linq;
using System.Reflection;
using KerbalOverEngineering.Options;

static class ParallaxTopologyContractTests
{
    static int Main(string[] args)
    {
        AppDomain.CurrentDomain.AssemblyResolve += delegate(object sender, ResolveEventArgs request) {
            string name = new AssemblyName(request.Name).Name + ".dll";
            foreach (string folder in new[] {"KSP_x64_Data/Managed", "GameData/000_KSPBurst/Plugins", "GameData/ParallaxContinued/Plugins", "GameData/000_Harmony"}) {
                string path = Path.Combine(args[0], folder, name);
                if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path));
            }
            return null;
        };
        var assembly = Assembly.Load(File.ReadAllBytes(Path.Combine(args[0], "GameData/ParallaxContinued/Plugins/ParallaxContinued.dll")));
        string problem=ParallaxTopologyContract.Check(assembly);
        if(problem!=null)throw new Exception(problem);
        if(ParallaxTopologyContract.Check(null)==null)throw new Exception("Missing Parallax must be rejected");
        byte[] bytes=File.ReadAllBytes(Path.Combine(args[0],"GameData/ParallaxContinued/Plugins/ParallaxContinued.dll"));
        byte[] old=assembly.ManifestModule.ModuleVersionId.ToByteArray();
        int found=0;
        for(int i=0;i<=bytes.Length-old.Length;i++) {
            if(bytes[i]!=old[0])continue;
            bool same=true;
            for(int j=1;j<old.Length;j++)if(bytes[i+j]!=old[j]){same=false;break;}
            if(!same)continue;
            Array.Copy(Guid.NewGuid().ToByteArray(),0,bytes,i,old.Length);found++;
        }
        if(found!=1)throw new Exception("Expected one MVID in private fixture");
        if(ParallaxTopologyContract.Check(Assembly.Load(bytes))!=null)throw new Exception("Equivalent code with different MVID must be supported");
        var incompatible=AppDomain.CurrentDomain.DefineDynamicAssembly(new AssemblyName("ParallaxIncompatible"),System.Reflection.Emit.AssemblyBuilderAccess.Run);
        if(ParallaxTopologyContract.Check(incompatible)==null)throw new Exception("Changed topology API must be rejected");
        Console.WriteLine("Parallax topology contract: installed API, missing API, changed API and MVID-independent compatibility passed.");
        return 0;
    }
}
