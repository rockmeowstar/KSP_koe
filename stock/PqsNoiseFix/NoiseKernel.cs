using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using System.Threading.Tasks;
using HarmonyLib;
using UnityEngine;

namespace PqsNoiseFix
{
    public static class NoiseKernel
    {
        const string Id="local.koe.pqs-noise-preparation";
        const int MaxPoints=300000;
        static long MaxValuesBytes { get { return KerbalOverEngineering.Options.LoadingResources.Current.NoiseBudget; } }
        internal struct Point:IEquatable<Point>
        {
            public long X,Y,Z;
            public Point(double x,double y,double z){X=BitConverter.DoubleToInt64Bits(x);Y=BitConverter.DoubleToInt64Bits(y);Z=BitConverter.DoubleToInt64Bits(z);}
            public bool Equals(Point p){return X==p.X&&Y==p.Y&&Z==p.Z;}
            public override bool Equals(object p){return p is Point&&Equals((Point)p);}
            public override int GetHashCode(){unchecked{return((X.GetHashCode()*31)+Y.GetHashCode())*31+Z.GetHashCode();}}
        }
        internal sealed class Generator
        {
            public Simplex Original;public int[] Perm;public long Octaves,Persistence,Frequency;public int ResultIndex;
            public bool Matches(Simplex s){return ReferenceEquals(Perm,readPerm(s))&&Octaves==BitConverter.DoubleToInt64Bits(s.octaves)&&Persistence==BitConverter.DoubleToInt64Bits(s.persistence)&&Frequency==BitConverter.DoubleToInt64Bits(s.frequency);}
            public bool SameKernel(Generator other){return Octaves==other.Octaves&&Persistence==other.Persistence&&Frequency==other.Frequency&&Perm.SequenceEqual(other.Perm);}
        }
        public sealed class Batch
        {
            internal string Sphere;internal bool Enabled,Ready;internal long Started,Calls,Hits,Misses,Checks,Errors,Quads;internal int Kernels;
            internal Dictionary<Point,int> Index;internal List<Point> Points;internal Generator[] Generators;
            internal Dictionary<Simplex,int> Owners;internal double[][] Values;internal double SnapshotSeconds,WorkerSeconds;
        }
        [ThreadStatic]static Batch current;
        static int mode;
        static bool invalid;
        static Func<Simplex,int[]> readPerm;
        static readonly MethodInfo clone=typeof(object).GetMethod("MemberwiseClone",BindingFlags.Instance|BindingFlags.NonPublic);
        static MethodInfo triple,vector,normalized;
        static MethodInfo[] guarded;
        static HarmonyPatchState patchState;
        static readonly Dictionary<MethodInfo,bool> knownKopernicus=new Dictionary<MethodInfo,bool>();
        static readonly HashSet<Type> heightTypes=new HashSet<Type>{typeof(PQSMod_VertexRidgedAltitudeCurve),typeof(PQSMod_VertexSimplexHeightAbsolute),typeof(PQSMod_VertexHeightNoiseVertHeightCurve2)};
        public static long Hits,Checks,Mismatches,Prepared;
        static Action<string,string> emit;
        static void Emit(string kind,string message){if(emit!=null)emit(kind,message);}
        static void Hash(MethodInfo method,string expected)
        {
            using(var sha=SHA256.Create())if(BitConverter.ToString(sha.ComputeHash(method.GetMethodBody().GetILAsByteArray())).Replace("-","").ToLowerInvariant()!=expected)throw new NotSupportedException("Unknown IL: "+method.Name);
        }
        internal static void Bind(out MethodInfo init,out MethodInfo[] callbacks)
        {
            if(typeof(PQS).Assembly.ManifestModule.ModuleVersionId!=new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba"))throw new NotSupportedException("Unknown PQS");
            triple=AccessTools.DeclaredMethod(typeof(Simplex),"noise",new[]{typeof(double),typeof(double),typeof(double)});
            vector=AccessTools.DeclaredMethod(typeof(Simplex),"noise",new[]{typeof(Vector3d)});
            normalized=AccessTools.DeclaredMethod(typeof(Simplex),"noiseNormalized",new[]{typeof(Vector3d)});
            var value=AccessTools.DeclaredMethod(typeof(Simplex),"value");init=AccessTools.DeclaredMethod(typeof(PQS),"UpdateQuadsInit");
            Hash(triple,"a934c87cca747f522b1388098c141ee25e6d231e49572ce3122f6a8731f98a87");
            Hash(value,"333dcf28bc47e9c583c98e9dd222fd1610054cbcff7d2b9c8bd69c6f6d21bb63");
            Hash(init,"38ec3fc1fa555bf9cc9add452b4a2522b8794f6492d937100f80eac15b108c95");
            callbacks=heightTypes.Select(t=>AccessTools.DeclaredMethod(t,"OnVertexBuildHeight")).Concat(new[]{AccessTools.DeclaredMethod(typeof(PQSLandControl),"OnVertexBuildHeight"),AccessTools.DeclaredMethod(typeof(PQSLandControl),"OnVertexBuild")}).ToArray();
            guarded=new[]{triple,vector,normalized,value,AccessTools.DeclaredMethod(typeof(Simplex),"noiseNormalized",new[]{typeof(double),typeof(double),typeof(double)})}.Concat(callbacks).ToArray();
        }
        public static void Apply(int selected,Action<string,string> logger,bool requirePatchState=false)
        {
            emit=logger;
            var h=new Harmony(Id);
            try{
                if(selected<1||selected>3)throw new ArgumentOutOfRangeException("KOE_PQS_NOISE_MODE");
                MethodInfo init;MethodInfo[] callbacks;Bind(out init,out callbacks);
                patchState=requirePatchState ? HarmonyPatchState.Bind() : null;
                if(requirePatchState&&patchState==null)throw new NotSupportedException("Unsupported Harmony patch-state implementation");
                if(!Compatible())throw new NotSupportedException("Other noise/callback patches present");
                var getter=new DynamicMethod("KOE_SimplexPermutation",typeof(int[]),new[]{typeof(Simplex)},typeof(Simplex),true);
                var il=getter.GetILGenerator();il.Emit(OpCodes.Ldarg_0);il.Emit(OpCodes.Ldfld,AccessTools.Field(typeof(Simplex),"perm"));il.Emit(OpCodes.Ret);
                readPerm=(Func<Simplex,int[]>)getter.CreateDelegate(typeof(Func<Simplex,int[]>));mode=selected;
                foreach(var callback in callbacks)h.Patch(callback,transpiler:new HarmonyMethod(typeof(NoiseKernel),"Callbacks"));
                h.Patch(init,prefix:new HarmonyMethod(typeof(NoiseKernel),"Begin"),transpiler:new HarmonyMethod(typeof(NoiseKernel),"Init"),finalizer:new HarmonyMethod(typeof(NoiseKernel),"End"));
                h.Patch(AccessTools.DeclaredMethod(typeof(PQS),"BuildQuad"),prefix:new HarmonyMethod(typeof(NoiseKernel),"CheckCompatibility"));
                Emit("PQS_NOISE_MODE","mode="+mode+"; 1=control; 2=bit oracle; 3=prepared; workers="+Environment.ProcessorCount+"; pointLimit="+MaxPoints+"; valueBudgetBytes="+MaxValuesBytes);
            }catch{h.UnpatchAll(Id);mode=0;throw;}
        }
        static bool KnownKopernicus(MethodInfo original,HarmonyLib.Patch patch)
        {
            var method=patch.PatchMethod;
            if(original.DeclaringType!=typeof(PQSLandControl)||original.Name!="OnVertexBuildHeight"||patch.owner!="Kopernicus"||method==null||
                method.DeclaringType.FullName!="Kopernicus.PQSLandControl_OnVertexBuildHeight"||method.Name!="Transpiler"||
                method.Module.ModuleVersionId!=new Guid("7fd28341-4f5a-4edf-b5f4-35706c820e0e")||Harmony.GetPatchInfo(method)!=null)return false;
            bool valid;if(knownKopernicus.TryGetValue(method,out valid))return valid;
            using(var sha=SHA256.Create())using(var file=File.OpenRead(method.Module.Assembly.Location))
                valid=BitConverter.ToString(sha.ComputeHash(file)).Replace("-","").ToLowerInvariant()=="968c7f371a376d5d22fd37f6e8891aeaf495cf9dd6f98bcc7b17c1935e42a9ff";
            knownKopernicus[method]=valid;return valid;
        }
        static bool Compatible()
        {
            return patchState==null?FullCompatibility():patchState.Validate(FullCompatibility,TrackedMethods);
        }
        static MethodBase[] TrackedMethods(){return guarded.Cast<MethodBase>().Concat(knownKopernicus.Keys.Cast<MethodBase>()).ToArray();}
        static bool FullCompatibility()
        {
            if(guarded==null)return false;
            foreach(var method in guarded){
                if(method==null)return false;var info=Harmony.GetPatchInfo(method);if(info==null)continue;
                if(info.Prefixes.Concat(info.Postfixes).Concat(info.Finalizers).Any(p=>p.owner!=Id))return false;
                if(info.Transpilers.Any(p=>p.owner!=Id&&!KnownKopernicus(method,p)))return false;
            }return true;
        }
        static string CompatibilityDetails()
        {
            return "cacheVertCount="+PQS.cacheVertCount+"; cacheVerts="+(PQS.cacheVerts==null?-1:PQS.cacheVerts.Length)+"; owners="+
                string.Join(" | ",guarded.Where(m=>m!=null&&Harmony.GetPatchInfo(m)!=null).Select(m=>m.DeclaringType.FullName+"."+m.Name+":"+string.Join(",",Harmony.GetPatchInfo(m).Owners.ToArray())).ToArray());
        }
        public static void CheckCompatibility(){if(current!=null&&current.Ready&&!Compatible()){current.Ready=false;invalid=true;Emit("PQS_NOISE_FALLBACK","late callback/noise patch");}}
        public static IEnumerable<CodeInstruction> Callbacks(IEnumerable<CodeInstruction> source,MethodBase __originalMethod)
        {
            int count=0;var code=source.Select(c=>new CodeInstruction(c)).ToList();
            foreach(var instruction in code){string replacement=null;
                if(instruction.Calls(triple))replacement="Triple";else if(instruction.Calls(vector))replacement="Vector";else if(instruction.Calls(normalized))replacement="Normalized";
                if(replacement!=null){instruction.opcode=OpCodes.Call;instruction.operand=AccessTools.Method(typeof(NoiseKernel),replacement);count++;}
            }
            int expected=__originalMethod.DeclaringType==typeof(PQSLandControl)&&__originalMethod.Name=="OnVertexBuildHeight"?4:1;
            if(count!=expected)throw new NotSupportedException("Noise call sites changed: "+__originalMethod+" count="+count);
            return code;
        }
        public static IEnumerable<CodeInstruction> Init(IEnumerable<CodeInstruction> source)
        {
            var code=source.Select(c=>new CodeInstruction(c)).ToList();var result=new List<CodeInstruction>();int count=0;
            var field=AccessTools.Field(typeof(PQS),"quadAllowBuild");
            for(int i=0;i<code.Count;i++){result.Add(code[i]);if(i>0&&code[i].opcode==OpCodes.Stfld&&Equals(code[i].operand,field)&&code[i-1].opcode==OpCodes.Ldc_I4_1){
                result.Add(new CodeInstruction(OpCodes.Ldarg_0));result.Add(new CodeInstruction(OpCodes.Call,AccessTools.Method(typeof(NoiseKernel),"Prepare")));count++;
            }}
            if(count!=1)throw new NotSupportedException("Unknown quad-build boundary");return result;
        }
        public static void Begin(PQS __instance,out Batch __state)
        {
            __state=current;current=new Batch{Sphere=__instance.name,Started=Stopwatch.GetTimestamp()};
        }
        public static Exception End(Batch __state,Exception __exception)
        {
            var batch=current;current=__state;
            if(batch!=null&&batch.Enabled){Hits+=batch.Hits;Checks+=batch.Checks;Mismatches+=batch.Errors;
                Emit("PQS_NOISE_BATCH","sphere="+batch.Sphere+"; mode="+mode+"; generators="+(batch.Generators==null?0:batch.Generators.Length)+"; kernels="+batch.Kernels+"; points="+(batch.Points==null?0:batch.Points.Count)+
                    "; quads="+batch.Quads+"; calls="+batch.Calls+"; hits="+batch.Hits+"; misses="+batch.Misses+"; checks="+batch.Checks+"; mismatches="+batch.Errors+
                    "; snapshotSeconds="+Seconds(batch.SnapshotSeconds)+"; workerSeconds="+Seconds(batch.WorkerSeconds)+"; totalQuadInitSeconds="+Seconds((Stopwatch.GetTimestamp()-batch.Started)/(double)Stopwatch.Frequency));
            }return __exception;
        }
        static string Seconds(double value){return value.ToString("F6",System.Globalization.CultureInfo.InvariantCulture);}
        static void AddGenerator(List<Simplex> list,Simplex instance){if(instance!=null&&!list.Contains(instance))list.Add(instance);}
        static Simplex[] Generators(PQS sphere)
        {
            var list=new List<Simplex>();var mods=(PQSMod[])AccessTools.Field(typeof(PQS),"mods").GetValue(sphere);
            if(mods==null)return list.ToArray();
            foreach(var modifier in mods){
                if(ReferenceEquals(modifier,null))continue;
                if(heightTypes.Contains(modifier.GetType()))AddGenerator(list,(Simplex)AccessTools.Field(modifier.GetType(),"simplex").GetValue(modifier));
                if(modifier.GetType()!=typeof(PQSLandControl))continue;
                foreach(var field in typeof(PQSLandControl).GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic))if(field.FieldType==typeof(Simplex))AddGenerator(list,(Simplex)field.GetValue(modifier));
                var classes=(Array)AccessTools.Field(typeof(PQSLandControl),"landClasses").GetValue(modifier);
                if(classes!=null)foreach(var landClass in classes){if(landClass==null)continue;foreach(var field in landClass.GetType().GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic))if(field.FieldType==typeof(Simplex))AddGenerator(list,(Simplex)field.GetValue(landClass));}
            }return list.ToArray();
        }
        static void Collect(PQ quad,Batch batch)
        {
            if(quad==null)return;
            if(quad.isSubdivided){foreach(var child in quad.subNodes)Collect(child,batch);return;}
            if(quad.isBuilt)return;batch.Quads++;
            for(int i=0;i<PQS.cacheVertCount;i++){
                Vector3d global=quad.quadMatrix.MultiplyPoint3x4(PQS.cacheVerts[i]);Vector3d direction=global.normalized;
                if(double.IsNaN(direction.x)||double.IsNaN(direction.y)||double.IsNaN(direction.z))throw new NotSupportedException("Invalid vertex direction");
                var point=new Point(direction.x,direction.y,direction.z);if(batch.Index.ContainsKey(point))continue;
                if(batch.Points.Count>=MaxPoints)throw new NotSupportedException("Coordinate budget exceeded");
                batch.Index.Add(point,batch.Points.Count);batch.Points.Add(point);
            }
        }
        public static void Prepare(PQS sphere)
        {
            var batch=current;if(batch==null||invalid)return;
            if(MaxValuesBytes<=0)return;
            try{
                long start=Stopwatch.GetTimestamp();var originals=Generators(sphere).Where(s=>s.octaves>=1&&s.octaves<=16&&!double.IsNaN(s.frequency)&&!double.IsInfinity(s.frequency)&&!double.IsNaN(s.persistence)&&!double.IsInfinity(s.persistence)&&readPerm(s)!=null&&readPerm(s).Length==512).Take(64).ToArray();
                if(originals.Length==0)return; // No supported generators: no work, compatibility scans or per-batch logs.
                batch.Enabled=true;if(mode==1)return;
                if(!Compatible()){invalid=true;throw new NotSupportedException("Unsupported active pipeline; "+CompatibilityDetails());}
                if(PQS.cacheVerts==null||PQS.cacheVertCount<1||PQS.cacheVertCount>PQS.cacheVerts.Length)throw new NotSupportedException("Invalid PQS vertex cache");
                batch.Generators=originals.Select(s=>new Generator{Original=s,Perm=readPerm(s),Octaves=BitConverter.DoubleToInt64Bits(s.octaves),Persistence=BitConverter.DoubleToInt64Bits(s.persistence),Frequency=BitConverter.DoubleToInt64Bits(s.frequency)}).ToArray();
                batch.Index=new Dictionary<Point,int>();batch.Points=new List<Point>();foreach(var quad in sphere.quads)Collect(quad,batch);
                if(batch.Points.Count==0)throw new NotSupportedException("No unbuilt leaf coordinates");
                int limit=(int)Math.Min(originals.Length,MaxValuesBytes/(batch.Points.Count*8L));if(limit<1)throw new NotSupportedException("Value budget exceeded");
                var unique=new List<Generator>();var accepted=new List<Generator>();
                foreach(var generator in batch.Generators){
                    int index=unique.FindIndex(g=>g.SameKernel(generator));
                    if(index<0){if(unique.Count>=limit)continue;index=unique.Count;unique.Add(generator);}
                    generator.ResultIndex=index;accepted.Add(generator);
                }
                batch.Generators=accepted.ToArray();var kernels=unique.ToArray();batch.Kernels=kernels.Length;long values=(long)batch.Points.Count*kernels.Length;
                batch.Values=kernels.Select(s=>new double[batch.Points.Count]).ToArray();batch.Owners=new Dictionary<Simplex,int>();for(int i=0;i<batch.Generators.Length;i++)batch.Owners.Add(batch.Generators[i].Original,i);
                batch.SnapshotSeconds=(Stopwatch.GetTimestamp()-start)/(double)Stopwatch.Frequency;start=Stopwatch.GetTimestamp();
                int workers=Math.Max(1,Math.Min(64,Environment.ProcessorCount));var permField=AccessTools.Field(typeof(Simplex),"perm");
                Parallel.For(0,workers,new ParallelOptions{MaxDegreeOfParallelism=workers},worker=>{
                    var copies=new Simplex[kernels.Length];for(int g=0;g<copies.Length;g++){copies[g]=(Simplex)clone.Invoke(kernels[g].Original,null);permField.SetValue(copies[g],(int[])kernels[g].Perm.Clone());}
                    int begin=(int)((long)batch.Points.Count*worker/workers),end=(int)((long)batch.Points.Count*(worker+1)/workers);
                    for(int i=begin;i<end;i++){var point=batch.Points[i];double x=BitConverter.Int64BitsToDouble(point.X),y=BitConverter.Int64BitsToDouble(point.Y),z=BitConverter.Int64BitsToDouble(point.Z);
                        for(int g=0;g<copies.Length;g++)batch.Values[g][i]=copies[g].noise(x,y,z);
                    }
                });
                batch.WorkerSeconds=(Stopwatch.GetTimestamp()-start)/(double)Stopwatch.Frequency;Prepared+=values;batch.Ready=true;
            }catch(Exception ex){batch.Ready=false;batch.Values=null;batch.Index=null;Emit("PQS_NOISE_FALLBACK",ex.GetType().Name+": "+ex.Message);}
        }
        static bool Find(Simplex generator,double x,double y,double z,out double value)
        {
            value=0;var batch=current;if(batch==null||!batch.Enabled)return false;batch.Calls++;
            if(!batch.Ready)return false;int owner,index;
            if(!batch.Owners.TryGetValue(generator,out owner)||!batch.Generators[owner].Matches(generator)||!batch.Index.TryGetValue(new Point(x,y,z),out index)){batch.Misses++;return false;}
            batch.Hits++;value=batch.Values[batch.Generators[owner].ResultIndex][index];return true;
        }
        static void Check(double prepared,double original)
        {
            var batch=current;batch.Checks++;if(BitConverter.DoubleToInt64Bits(prepared)!=BitConverter.DoubleToInt64Bits(original)){batch.Errors++;invalid=true;batch.Ready=false;}
        }
        public static double Triple(Simplex generator,double x,double y,double z)
        {
            double prepared;if(!Find(generator,x,y,z,out prepared))return generator.noise(x,y,z);
            if(mode==3)return prepared;double original=generator.noise(x,y,z);Check(prepared,original);return original;
        }
        public static double Vector(Simplex generator,Vector3d point)
        {
            double prepared;if(!Find(generator,point.x,point.y,point.z,out prepared))return generator.noise(point);
            if(mode==3)return prepared;double original=generator.noise(point);Check(prepared,original);return original;
        }
        public static double Normalized(Simplex generator,Vector3d point)
        {
            double prepared;if(!Find(generator,point.x,point.y,point.z,out prepared))return generator.noiseNormalized(point);
            prepared=(prepared+1.0)*.5;if(mode==3)return prepared;double original=generator.noiseNormalized(point);Check(prepared,original);return original;
        }
        public static void Report(){if(mode>0)Emit("PQS_NOISE_TOTAL","mode="+mode+"; prepared="+Prepared+"; hits="+Hits+"; checks="+Checks+"; mismatches="+Mismatches+"; compatibilityFastChecks="+(patchState==null?0:patchState.FastChecks)+"; compatibilityFullChecks="+(patchState==null?0:patchState.FullChecks)+"; invalid="+invalid);}
    }
}
