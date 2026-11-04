using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Serialization;
using System.Threading;
using System.Threading.Tasks;

static class PqsNoiseCloneTests
{
    static string game;static long checks;
    static Assembly Resolve(object sender,ResolveEventArgs args)
    {
        string path=Path.Combine(game,@"KSP_x64_Data\Managed",new AssemblyName(args.Name).Name+".dll");
        return File.Exists(path)?Assembly.Load(File.ReadAllBytes(path)):null;
    }
    static int Main(string[] args)
    {
        try{game=args[0];AppDomain.CurrentDomain.AssemblyResolve+=Resolve;Run();Console.WriteLine("PQS_SIMPLEX_CLONE_BIT_PARITY_PASS checks="+checks+"; original KSP IL; no Unity constructor/random calls; Unity Mono batch integration untested");return 0;}
        catch(Exception ex){Console.Error.WriteLine(ex);return 1;}
    }
    [MethodImpl(MethodImplOptions.NoInlining)]static void Run()
    {
        if(typeof(Simplex).Assembly.ManifestModule.ModuleVersionId!=new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))throw new NotSupportedException("Unknown Simplex");
        var perm=typeof(Simplex).GetField("perm",BindingFlags.Instance|BindingFlags.NonPublic);
        var clone=typeof(object).GetMethod("MemberwiseClone",BindingFlags.Instance|BindingFlags.NonPublic);
        double[,] configs={{1,.5,1},{4,.5,1},{6.5,.7,3.9},{16,.3,1},{3,-.4,-2},{4,1,0},{2,.5,31},{8,.1,.0001}};
        const int count=4104;var x=new double[count];var y=new double[count];var z=new double[count];var random=new Random(705310);
        for(int i=0;i<count;i++){x[i]=random.NextDouble()*2-1;y[i]=random.NextDouble()*2-1;z[i]=random.NextDouble()*2-1;}
        double[] edges={0.0,-0.0,1.0,-1.0,double.Epsilon,-double.Epsilon,.5,-.5};
        for(int i=0;i<edges.Length;i++){x[i]=edges[i];y[i]=edges[(i+3)%edges.Length];z[i]=edges[(i+5)%edges.Length];}
        for(int permutation=0;permutation<4;permutation++)for(int configuration=0;configuration<configs.GetLength(0);configuration++){
            // Bypass the constructor because it reseeds UnityEngine.Random. The pure
            // method writes its scratch fields; only permutation/config are inputs.
            var original=(Simplex)FormatterServices.GetUninitializedObject(typeof(Simplex));
            var table=new int[512];for(int i=0;i<256;i++)table[i]=permutation==0?i:random.Next(256);
            Array.Copy(table,0,table,256,256);perm.SetValue(original,table);
            original.octaves=configs[configuration,0];original.persistence=configs[configuration,1];original.frequency=configs[configuration,2];
            var expected=new long[count];var normalized=new long[count];
            for(int i=0;i<count;i++){
                expected[i]=BitConverter.DoubleToInt64Bits(original.noise(x[i],y[i],z[i]));
                normalized[i]=BitConverter.DoubleToInt64Bits(original.noiseNormalized(x[i],y[i],z[i]));
            }
            Parallel.For<Simplex>(0,count,new ParallelOptions{MaxDegreeOfParallelism=8},()=>{
                var copy=(Simplex)clone.Invoke(original,null);perm.SetValue(copy,(int[])table.Clone());return copy;
            },(i,state,copy)=>{
                if(BitConverter.DoubleToInt64Bits(copy.noise(x[i],y[i],z[i]))!=expected[i])throw new Exception("Raw noise mismatch at "+i);
                if(BitConverter.DoubleToInt64Bits(copy.noiseNormalized(x[i],y[i],z[i]))!=normalized[i])throw new Exception("Normalized noise mismatch at "+i);
                Interlocked.Add(ref checks,2);return copy;
            },copy=>{});
        }
    }
}
