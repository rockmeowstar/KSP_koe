using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using HarmonyLib;
using Parallax;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using Unity.Jobs;
using Unity.Mathematics;
using UnityEngine;

namespace KerbalOverEngineering.ParallaxTopology
{
    public sealed class Bootstrap : MonoBehaviour
    {
        const string PatchId="KerbalOverEngineering.ParallaxTopology";
        const string OptionKey="ParallaxContinued.CertifiedMeshReuse";
        [DllImport("kernel32",CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
        [StructLayout(LayoutKind.Sequential)] struct Stats { public ulong Visits,Distances,Midpoints,Leaves,Requests; }
        [StructLayout(LayoutKind.Sequential)] struct Output {
            public IntPtr Vertices,Indices; public UIntPtr VertexCount,IndexCount;
            public uint Width; public ulong Revision; public Stats Stats;
        }
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern int koe_topology_seal_fast(IntPtr context);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern int koe_topology_can_reuse(IntPtr context,float[] target,uint depth,float[] levels,UIntPtr count,ParallaxPlane[] planes,float bypass);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern IntPtr koe_topology_create(IntPtr source,UIntPtr count,UIntPtr maxNodes,UIntPtr outputLimit);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern int koe_topology_prepare(IntPtr context,float[] target,uint depth,float[] levels,UIntPtr count,ParallaxPlane[] planes,float bypass);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern int koe_topology_range(IntPtr context,UIntPtr begin,UIntPtr end);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern int koe_topology_finish(IntPtr context,out Output result);
        [DllImport("KoeParallaxTopology",CallingConvention=CallingConvention.Cdecl)]
        static extern void koe_topology_destroy(IntPtr context);

        static class Fields {
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeArray<SubdividableTriangle>> Source=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeArray<SubdividableTriangle>>("meshTriangles");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeArray<float>> Levels=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeArray<float>>("sqrLevelThresholds");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,ParallaxPlane[]> Planes=AccessTools.FieldRefAccess<JobifiedSubdivision,ParallaxPlane[]>("localFrustumPlanes");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeArray<ParallaxPlane>> JobPlanes=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeArray<ParallaxPlane>>("frustumPlanes");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,float3[]> LastNormals=AccessTools.FieldRefAccess<JobifiedSubdivision,float3[]>("lastLocalPlaneNormals");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,float3> LastTarget=AccessTools.FieldRefAccess<JobifiedSubdivision,float3>("lastLocalTarget");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,int> LastDepth=AccessTools.FieldRefAccess<JobifiedSubdivision,int>("lastMaxSubdivisionLevel");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,float> LastRange=AccessTools.FieldRefAccess<JobifiedSubdivision,float>("lastSubdivisionRange");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,bool> Force=AccessTools.FieldRefAccess<JobifiedSubdivision,bool>("forceDispatch");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,bool> Running=AccessTools.FieldRefAccess<JobifiedSubdivision,bool>("isSubdividing");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,JobHandle> Handle=AccessTools.FieldRefAccess<JobifiedSubdivision,JobHandle>("subdivisionJobHandle");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeList<float3>> Positions=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeList<float3>>("newVerts"),Normals=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeList<float3>>("newNormals");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeList<float4>> Colors=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeList<float4>>("newColors");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeList<int>> Indices=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeList<int>>("newTriangles");
            internal static readonly AccessTools.FieldRef<JobifiedSubdivision,NativeList<ushort>> Shorts=AccessTools.FieldRefAccess<JobifiedSubdivision,NativeList<ushort>>("newShortTriangles");
            internal static readonly Action<JobifiedSubdivision,float> Thresholds=AccessTools.MethodDelegate<Action<JobifiedSubdivision,float>>(AccessTools.Method(typeof(JobifiedSubdivision),"UpdateLevelThresholds"));
            internal static readonly float Bypass=(float)AccessTools.Field(typeof(JobifiedSubdivision),"frustumCullBypassRange").GetRawConstantValue();
        }
        sealed class State : IDisposable {
            internal IntPtr Context;
            internal NativeArray<int> Roots,Success;
            internal NativeArray<Output> Result;
            internal JobHandle Handle;
            internal bool Pending,Disabled,BuildFromNative,HasMesh;
            internal long Reservation;
            internal int SourceCount;
            internal readonly float[] Target=new float[3];
            internal float[] Levels;
            public void Dispose(){
                Handle.Complete();Pending=false;HasMesh=false;
                if(Context!=IntPtr.Zero){koe_topology_destroy(Context);Context=IntPtr.Zero;}
                if(Roots.IsCreated)Roots.Dispose();if(Success.IsCreated)Success.Dispose();if(Result.IsCreated)Result.Dispose();
                reserved-=Reservation;Reservation=0;
            }
        }
        struct RootsJob : IJobParallelFor {
            internal IntPtr Context;internal int Count;internal NativeArray<int> Status;
            public void Execute(int index){int begin=index*4;Status[index]=koe_topology_range(Context,(UIntPtr)begin,(UIntPtr)Math.Min(begin+4,Count));}
        }
        struct FinishJob : IJob {
            internal IntPtr Context;
            internal bool Reuse;
            [ReadOnly] internal NativeArray<int> Roots;
            internal NativeArray<int> Success;
            internal NativeArray<Output> Result;
            internal NativeList<float3> Positions,Normals;
            internal NativeList<float4> Colors;
            internal NativeList<int> Indices;
            internal NativeList<ushort> Shorts;
            public unsafe void Execute(){
                Success[0]=0;
                try{
                    for(int i=0;i<Roots.Length;i++)if(Roots[i]!=1)return;
                    Output output;if(koe_topology_finish(Context,out output)!=1)return;
                    int vertices=checked((int)output.VertexCount.ToUInt64()),indices=checked((int)output.IndexCount.ToUInt64());
                    if(output.Width!=2&&output.Width!=4)return;
                    Positions.ResizeUninitialized(vertices);Normals.ResizeUninitialized(vertices);Colors.ResizeUninitialized(vertices);
                    if(output.Width==2){Shorts.ResizeUninitialized(indices);Indices.Clear();}
                    else{Indices.ResizeUninitialized(indices);Shorts.Clear();}
                    {
                        byte* source=(byte*)output.Vertices;
                        UnsafeUtility.MemCpyStride(NativeArrayUnsafeUtility.GetUnsafePtr(Positions.AsArray()),12,source,40,12,vertices);
                        UnsafeUtility.MemCpyStride(NativeArrayUnsafeUtility.GetUnsafePtr(Normals.AsArray()),12,source+12,40,12,vertices);
                        UnsafeUtility.MemCpyStride(NativeArrayUnsafeUtility.GetUnsafePtr(Colors.AsArray()),16,source+24,40,16,vertices);
                        if(output.Width==2)UnsafeUtility.MemCpy(NativeArrayUnsafeUtility.GetUnsafePtr(Shorts.AsArray()),(void*)output.Indices,indices*2L);
                        else UnsafeUtility.MemCpy(NativeArrayUnsafeUtility.GetUnsafePtr(Indices.AsArray()),(void*)output.Indices,indices*4L);
                    }
                    if(Reuse&&koe_topology_seal_fast(Context)!=1)return;
                    Result[0]=output;Success[0]=1;
                }catch(Exception){Success[0]=0;}
            }
        }
        static readonly Dictionary<JobifiedSubdivision,State> states=new Dictionary<JobifiedSubdivision,State>();
        static bool candidateEnabled,certifiedReuse,stopping;
        static long reuseChecks,reusedDispatches;
        static long budget,reserved,peakReserved,dispatches,nativeDispatches,nativeBuilds,originalBuilds,failures,budgetSkips;
        static ulong distances,requests;
        Harmony harmony;

        void Awake(){
            if(!KerbalOverEngineering.Options.Settings.Enabled(OptionKey)){Destroy(this);return;}
            try{
                if(Environment.OSVersion.Platform!=PlatformID.Win32NT||IntPtr.Size!=8)throw new PlatformNotSupportedException("Windows x64 native kernel required");
                string problem=KerbalOverEngineering.Options.ParallaxTopologyContract.Check(typeof(JobifiedSubdivision).Assembly);
                if(problem!=null)throw new NotSupportedException(problem);
                string directory=KerbalOverEngineering.Options.NativeAssets.Extract(Assembly.GetExecutingAssembly());
                if(LoadLibraryW(Path.Combine(directory,"KoeParallaxTopology.dll"))==IntPtr.Zero)throw new InvalidOperationException("Native topology library could not be loaded");
                candidateEnabled=true;certifiedReuse=true;stopping=false;
                budget=Math.Min((long)SystemInfo.systemMemorySize*1024*1024/128,256L*1024*1024);
                if(Marshal.SizeOf(typeof(Output))!=88||Marshal.SizeOf(typeof(SubdividableTriangle))!=120||Marshal.SizeOf(typeof(ParallaxPlane))!=16)throw new InvalidOperationException("Topology ABI");
                // AI-generated comment: Resolve all field accessors before installing patches.
                var warm=Fields.Handle;
                DontDestroyOnLoad(gameObject);harmony=new Harmony(PatchId);
                harmony.Patch(AccessTools.Method(typeof(JobifiedSubdivision),"DispatchSubdivision"),prefix:new HarmonyMethod(typeof(Bootstrap),"Dispatch"));
                harmony.Patch(AccessTools.Method(typeof(JobifiedSubdivision),"Update"),prefix:new HarmonyMethod(typeof(Bootstrap),"BeforeUpdate"));
                harmony.Patch(AccessTools.Method(typeof(JobifiedSubdivision),"BuildMesh"),postfix:new HarmonyMethod(typeof(Bootstrap),"Built"));
                harmony.Patch(AccessTools.Method(typeof(JobifiedSubdivision),"RefreshSourceData"),prefix:new HarmonyMethod(typeof(Bootstrap),"Retire"));
                harmony.Patch(AccessTools.Method(typeof(JobifiedSubdivision),"Cleanup"),prefix:new HarmonyMethod(typeof(Bootstrap),"Retire"));
                Debug.Log("[KOE Parallax] Certified mesh reuse enabled; cacheBudget="+budget);
            }catch(Exception e){if(harmony!=null)harmony.UnpatchAll(PatchId);Debug.LogException(e);Destroy(this);}
        }
        static unsafe State Create(JobifiedSubdivision owner){
            var source=Fields.Source(owner);int nodes=checked(source.Length*16);
            // AI-generated comment: The reservation includes retained output and hash capacity.
            long bytes=checked((long)nodes*1024);
            var state=new State{SourceCount=source.Length};
            if(bytes>budget-reserved){state.Disabled=true;budgetSkips++;return state;}
            state.Reservation=bytes;reserved+=bytes;peakReserved=Math.Max(peakReserved,reserved);
            try{
                state.Context=koe_topology_create((IntPtr)NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(source),(UIntPtr)source.Length,(UIntPtr)nodes,(UIntPtr)nodes);
                if(state.Context==IntPtr.Zero)throw new InvalidOperationException("Native topology create failed");
                state.Roots=new NativeArray<int>((source.Length+3)/4,Allocator.Persistent);
                state.Success=new NativeArray<int>(1,Allocator.Persistent);state.Result=new NativeArray<Output>(1,Allocator.Persistent);
                return state;
            }catch{state.Dispose();state.Disabled=true;failures++;return state;}
        }
        static bool Dispatch(JobifiedSubdivision __instance,ref float3 localTarget,float scale){
            dispatches++;if(!candidateEnabled||stopping)return true;
            State state;if(!states.TryGetValue(__instance,out state)){state=Create(__instance);states.Add(__instance,state);}
            if(state.Disabled)return true;
            try{
                Fields.Thresholds(__instance,scale);var levels=Fields.Levels(__instance);
                if(state.Levels==null||state.Levels.Length!=levels.Length)state.Levels=new float[levels.Length];
                levels.CopyTo(state.Levels);state.Target[0]=localTarget.x;state.Target[1]=localTarget.y;state.Target[2]=localTarget.z;
                var planes=Fields.Planes(__instance);Fields.JobPlanes(__instance).CopyFrom(planes);float bypass=Fields.Bypass/scale;
                if(certifiedReuse&&state.HasMesh&&!state.Pending&&!Fields.Running(__instance)&&!Fields.Force(__instance)){
                    reuseChecks++;
                    if(koe_topology_can_reuse(state.Context,state.Target,(uint)__instance.maxSubdivisionLevel,state.Levels,(UIntPtr)state.Levels.Length,planes,bypass*bypass)==1){
                        RememberQuery(__instance,localTarget,planes);reusedDispatches++;return false;
                    }
                }
                state.HasMesh=false;
                if(koe_topology_prepare(state.Context,state.Target,(uint)__instance.maxSubdivisionLevel,state.Levels,(UIntPtr)state.Levels.Length,planes,bypass*bypass)!=1)throw new InvalidOperationException("Native topology prepare failed");
                state.Success[0]=0;
                state.Handle=new RootsJob{Context=state.Context,Count=state.SourceCount,Status=state.Roots}.Schedule(state.Roots.Length,1);
                state.Handle=new FinishJob{Context=state.Context,Reuse=certifiedReuse,Roots=state.Roots,Success=state.Success,Result=state.Result,Positions=Fields.Positions(__instance),Normals=Fields.Normals(__instance),Colors=Fields.Colors(__instance),Indices=Fields.Indices(__instance),Shorts=Fields.Shorts(__instance)}.Schedule(state.Handle);
                RememberQuery(__instance,localTarget,planes);
                Fields.Force(__instance)=false;Fields.Handle(__instance)=state.Handle;Fields.Running(__instance)=true;
                state.Pending=true;nativeDispatches++;return false;
            }catch(Exception e){state.Dispose();state.Disabled=true;failures++;Debug.LogWarning("[KOE Topology] Original fallback: "+e.Message);return true;}
        }
        static void RememberQuery(JobifiedSubdivision owner,float3 target,ParallaxPlane[] planes){
            Fields.LastTarget(owner)=target;Fields.LastDepth(owner)=owner.maxSubdivisionLevel;Fields.LastRange(owner)=owner.subdivisionRange;
            var normals=Fields.LastNormals(owner);for(int i=0;i<6;i++)normals[i]=math.normalizesafe(planes[i].normal);
        }
        static void BeforeUpdate(JobifiedSubdivision __instance){
            State state;if(!candidateEnabled||!states.TryGetValue(__instance,out state)||!state.Pending||!state.Handle.IsCompleted)return;
            state.Handle.Complete();state.Pending=false;
            if(state.Success[0]!=1){
                state.Dispose();state.Disabled=true;Fields.Running(__instance)=false;Fields.Force(__instance)=true;failures++;
            }else{state.BuildFromNative=true;distances+=state.Result[0].Stats.Distances;requests+=state.Result[0].Stats.Requests;}
        }
        static void Built(JobifiedSubdivision __instance){
            State state;if(states.TryGetValue(__instance,out state)&&state.BuildFromNative){nativeBuilds++;state.BuildFromNative=false;state.HasMesh=certifiedReuse;}else originalBuilds++;
        }
        static void Retire(JobifiedSubdivision __instance){State state;if(states.TryGetValue(__instance,out state)){state.Dispose();states.Remove(__instance);}}
        void Report(){
            Debug.Log("[KOE Parallax] builds="+nativeBuilds+"; reuseChecks="+reuseChecks+"; reused="+reusedDispatches+"; failures="+failures+"; reservedBytes="+reserved);
        }
        void OnDestroy(){
            if(harmony==null)return;stopping=true;
            foreach(var state in states.Values)state.Dispose();states.Clear();Report();harmony.UnpatchAll(PatchId);
        }
    }
}
