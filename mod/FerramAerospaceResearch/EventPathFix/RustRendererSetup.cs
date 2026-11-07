using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Threading;
using HarmonyLib;
using UnityEngine;

namespace KoeFarEventPath
{
    internal static class RustRendererSetup
    {
        const string Id="KerbalOverEngineering.FAR.RustRendererSetup";
        [DllImport("kernel32",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
        [StructLayout(LayoutKind.Sequential)] struct Api { internal IntPtr SetColor,SetBlock; }
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Kernel(IntPtr renderers,UIntPtr count,IntPtr block,int property,ref Color color,ref Api api);
        static Kernel kernel;
        static Api api;
        static int exposedColor;
        static int mainThread;
        static MethodInfo setupMethod;
        static MethodInfo[] watched;
        static Func<MaterialPropertyBlock,IntPtr> blockPointer;
        static Func<object,Part,List<Renderer>,MaterialPropertyBlock,Color> original;
        static Func<object,Part,Color> colorFor;
        static Func<object,Dictionary<Part,int>> objects;
        static Func<object,List<HashSet<Renderer>>> sets;
        static IEqualityComparer<Renderer> rendererComparer;
        static IEqualityComparer<Part> partComparer;
        [ThreadStatic] static bool allowed;
        [ThreadStatic] static Stack<IntPtr[]> buffers;
        public static long Batches,Renderers,Fallbacks,Fixtures;

        internal static void Apply()
        {
            if(Environment.GetEnvironmentVariable("KOE_FAR_RUST_SETUP")!="1"&&!KerbalOverEngineering.Options.Settings.Enabled("FAR.RustRendererSetup"))return;
            var harmony=new Harmony(Id);
            try
            {
                if(IntPtr.Size!=8||Environment.OSVersion.Platform!=PlatformID.Win32NT)throw new NotSupportedException("Reviewed property binding requires Windows x64");
                mainThread=Thread.CurrentThread.ManagedThreadId;
                var renderer=AccessTools.TypeByName("FerramAerospaceResearch.Geometry.Exposure.Renderer`1").MakeGenericType(typeof(Part));
                if(RustRendererFilter.Hash(File.ReadAllBytes(renderer.Assembly.Location))!="2ce1e8a5093b9635bcd34941597fab4f60a8bc9c44a30dd9283725317d68979f")
                    throw new NotSupportedException("FAR renderer setup has not been reviewed");
                setupMethod=AccessTools.DeclaredMethod(renderer,"SetupRenderers").MakeGenericMethod(typeof(List<Renderer>));
                var getColor=AccessTools.DeclaredMethod(renderer,"GetObjColor");
                rendererComparer=DefaultComparer<Renderer>(renderer.Assembly);partComparer=DefaultComparer<Part>(renderer.Assembly);
                watched=new[]{setupMethod,getColor,AccessTools.Method(typeof(UnityEngine.Object),"GetHashCode"),AccessTools.Method(typeof(HashSet<Renderer>),"Add"),
                    AccessTools.Method(typeof(MaterialPropertyBlock),"SetColor",new[]{typeof(int),typeof(Color)}),AccessTools.Method(typeof(Renderer),"SetPropertyBlock",new[]{typeof(MaterialPropertyBlock)}),
                    AccessTools.Method(rendererComparer.GetType(),"GetHashCode",new[]{typeof(Renderer)}),AccessTools.Method(rendererComparer.GetType(),"Equals",new[]{typeof(Renderer),typeof(Renderer)}),
                    AccessTools.Method(partComparer.GetType(),"GetHashCode",new[]{typeof(Part)}),AccessTools.Method(partComparer.GetType(),"Equals",new[]{typeof(Part),typeof(Part)})};
                if(!Compatible())throw new NotSupportedException("FAR setup or property API is already patched");
                original=BindCall<Func<object,Part,List<Renderer>,MaterialPropertyBlock,Color>>(renderer,setupMethod,typeof(Color),typeof(object),typeof(Part),typeof(List<Renderer>),typeof(MaterialPropertyBlock));
                colorFor=BindCall<Func<object,Part,Color>>(renderer,getColor,typeof(Color),typeof(object),typeof(Part));
                objects=BindField<Dictionary<Part,int>>(renderer,"objects");sets=BindField<List<HashSet<Renderer>>>(renderer,"renderers");
                var field=AccessTools.Field(typeof(MaterialPropertyBlock),"m_Ptr");
                if(field==null||field.FieldType!=typeof(IntPtr))throw new NotSupportedException("Material property pointer field differs");
                var read=new DynamicMethod("KOE_BlockPointer",typeof(IntPtr),new[]{typeof(MaterialPropertyBlock)},typeof(RustRendererSetup),true);
                var il=read.GetILGenerator();il.Emit(OpCodes.Ldarg_0);il.Emit(OpCodes.Ldfld,field);il.Emit(OpCodes.Ret);
                blockPointer=(Func<MaterialPropertyBlock,IntPtr>)read.CreateDelegate(typeof(Func<MaterialPropertyBlock,IntPtr>));
                byte[] engine=RustRendererFilter.ReadEngine();
                api=new Api{SetColor=EngineFunction(engine,0x7c8770,239),SetBlock=EngineFunction(engine,0x670560,65)};
                string directory=KerbalOverEngineering.Options.NativeAssets.Extract(Assembly.GetExecutingAssembly());
                RustRendererFilter.LoadKernel(Path.Combine(directory,"KoeFarRendererFilter.dll"));
                IntPtr entry=RustRendererFilter.Entry("KoeSetupRendererProperties");
                if(entry==IntPtr.Zero)throw new NotSupportedException("Rust property setup export is missing");
                kernel=(Kernel)Marshal.GetDelegateForFunctionPointer(entry,typeof(Kernel));
                exposedColor=(int)AccessTools.Field(AccessTools.TypeByName("FerramAerospaceResearch.Resources.ShaderPropertyIds"),"_ExposedColor").GetValue(null);
                CheckFixtures();
                var exposure=AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.VehicleExposure");
                var reset=AccessTools.DeclaredMethod(exposure,"ResetParts").MakeGenericMethod(typeof(List<Part>));
                harmony.Patch(reset,new HarmonyMethod(typeof(RustRendererSetup),"Before"),null,new HarmonyMethod(typeof(RustRendererSetup),"Rewrite"),new HarmonyMethod(typeof(RustRendererSetup),"After"));
                Application.quitting+=()=>Debug.Log("[KOE FAR RustSetup] batches="+Batches+"; renderers="+Renderers+"; fallbacks="+Fallbacks+"; fixtures="+Fixtures);
                Debug.Log("[KOE FAR RustSetup] Enabled; fixtures="+Fixtures);
            }
            catch(Exception error){harmony.UnpatchAll(Id);Debug.LogError("[KOE FAR RustSetup] Disabled: "+error);}
        }
        internal static IEqualityComparer<T> DefaultComparer<T>(Assembly assembly)
        {
            var type=assembly.GetType("FerramAerospaceResearch.ObjectReferenceEqualityComparer`1",true).MakeGenericType(typeof(T));
            var getter=AccessTools.DeclaredPropertyGetter(type,"Default");
            if(getter==null||Harmony.GetPatchInfo(getter)!=null)throw new NotSupportedException("FAR reference comparer factory is already patched");
            var comparer=(IEqualityComparer<T>)getter.Invoke(null,null);
            if(comparer.GetType()!=type)throw new NotSupportedException("FAR reference comparer was replaced");
            return comparer;
        }
        static T BindCall<T>(Type owner,MethodInfo method,Type result,params Type[] arguments)
        {
            var call=new DynamicMethod("KOE_Far_"+method.Name,result,arguments,typeof(RustRendererSetup),true);
            var il=call.GetILGenerator();il.Emit(OpCodes.Ldarg_0);il.Emit(OpCodes.Castclass,owner);
            for(int i=1;i<arguments.Length;i++)il.Emit(OpCodes.Ldarg,(short)i);
            il.Emit(OpCodes.Call,method);il.Emit(OpCodes.Ret);
            return (T)(object)call.CreateDelegate(typeof(T));
        }
        static Func<object,T> BindField<T>(Type owner,string name)
        {
            var field=AccessTools.Field(owner,name);
            if(field==null||field.FieldType!=typeof(T))throw new NotSupportedException("FAR field differs: "+name);
            var read=new DynamicMethod("KOE_Far_"+name,typeof(T),new[]{typeof(object)},typeof(RustRendererSetup),true);
            var il=read.GetILGenerator();il.Emit(OpCodes.Ldarg_0);il.Emit(OpCodes.Castclass,owner);il.Emit(OpCodes.Ldfld,field);il.Emit(OpCodes.Ret);
            return (Func<object,T>)read.CreateDelegate(typeof(Func<object,T>));
        }
        static bool Compatible(){foreach(var method in watched)if(method==null||Harmony.GetPatchInfo(method)!=null)return false;return true;}
        static void Before(out bool __state){__state=allowed;allowed=Thread.CurrentThread.ManagedThreadId==mainThread&&Compatible();}
        static Exception After(bool __state,Exception __exception){allowed=__state;return __exception;}
        static IntPtr EngineFunction(byte[] image,int rva,int length)
        {
            int pe=BitConverter.ToInt32(image,60),count=BitConverter.ToUInt16(image,pe+6),section=pe+24+BitConverter.ToUInt16(image,pe+20);
            for(int i=0;i<count;i++,section+=40)
            {
                int start=BitConverter.ToInt32(image,section+12),size=BitConverter.ToInt32(image,section+16);
                if(rva<start||rva+length>start+size)continue;
                IntPtr module=GetModuleHandleW("UnityPlayer.dll");
                if(module==IntPtr.Zero)throw new NotSupportedException("Unity native module is not loaded");
                IntPtr target=new IntPtr(module.ToInt64()+rva);
                var actual=new byte[length];Marshal.Copy(target,actual,0,length);
                int offset=BitConverter.ToInt32(image,section+20)+rva-start;
                for(int b=0;b<length;b++)if(actual[b]!=image[offset+b])throw new NotSupportedException("Unity property function is already hooked");
                return target;
            }
            throw new NotSupportedException("Unity property function is outside image sections");
        }
        internal static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> code)
        {
            int changed=0;
            foreach(var instruction in code)
            {
                var copy=new CodeInstruction(instruction);
                if(copy.Calls(setupMethod)){copy.opcode=OpCodes.Call;copy.operand=AccessTools.Method(typeof(RustRendererSetup),"Setup");changed++;}
                yield return copy;
            }
            if(changed!=1)throw new NotSupportedException("Expected one renderer setup call in FAR reset");
        }
        public static unsafe Color Setup(object owner,Part part,List<Renderer> renderers,MaterialPropertyBlock block)
        {
            if(!allowed||block==null||renderers==null||renderers.GetType()!=typeof(List<Renderer>)){Fallbacks++;return original(owner,part,renderers,block);}
            if(buffers==null)buffers=new Stack<IntPtr[]>();
            var pointers=buffers.Count==0?new IntPtr[renderers.Count]:buffers.Pop();
            if(pointers.Length<renderers.Count)pointers=new IntPtr[renderers.Count];
            try
            {
                for(int i=0;i<renderers.Count;i++)
                {
                    var type=ReferenceEquals(renderers[i],null)?null:renderers[i].GetType();
                    if(type!=typeof(MeshRenderer)&&type!=typeof(SkinnedMeshRenderer)&&type!=typeof(LineRenderer)&&type!=typeof(TrailRenderer)&&type!=typeof(ParticleSystemRenderer))
                    {Fallbacks++;return original(owner,part,renderers,block);}
                    pointers[i]=RustRendererFilter.CachedPointer(renderers[i]);
                    if(pointers[i]==IntPtr.Zero){Fallbacks++;return original(owner,part,renderers,block);}
                }
                IntPtr sheet=blockPointer(block);
                if(sheet==IntPtr.Zero){Fallbacks++;return original(owner,part,renderers,block);}
                var map=objects(owner);
                if(!ReferenceEquals(map.Comparer,partComparer)){Fallbacks++;return original(owner,part,renderers,block);}
                Color color=colorFor(owner,part);
                int index=map[part];
                var list=sets(owner);
                if(!ReferenceEquals(list[index].Comparer,rendererComparer)){Fallbacks++;return original(owner,part,renderers,block);}
                int result;
                fixed(IntPtr* input=pointers)result=kernel((IntPtr)input,(UIntPtr)(uint)renderers.Count,sheet,exposedColor,ref color,ref api);
                if(result!=0)throw new InvalidOperationException("Prevalidated native property batch was rejected");
                foreach(var renderer in renderers)list[index].Add(renderer);
                Batches++;Renderers+=renderers.Count;
                return color;
            }
            finally{buffers.Push(pointers);GC.KeepAlive(block);GC.KeepAlive(renderers);}
        }
        static unsafe void CheckFixtures()
        {
            var root=new GameObject("KOE owned property fixture");root.hideFlags=HideFlags.HideAndDontSave;
            try
            {
                int marker=Shader.PropertyToID("_KOE_FixtureMarker");
                int vectorMarker=Shader.PropertyToID("_KOE_FixtureVector"),matrixMarker=Shader.PropertyToID("_KOE_FixtureMatrix"),textureMarker=Shader.PropertyToID("_KOE_FixtureTexture");
                var vector=new Vector4(1.25f,-2.5f,0,17);var matrix=Matrix4x4.identity;matrix.m03=3.5f;
                var block=new MaterialPropertyBlock();var read=new MaterialPropertyBlock();var reference=new MaterialPropertyBlock();
                foreach(var type in new[]{typeof(MeshRenderer),typeof(SkinnedMeshRenderer),typeof(LineRenderer),typeof(TrailRenderer),typeof(ParticleSystemRenderer)})
                {
                    var child=new GameObject("KOE owned "+type.Name);child.transform.SetParent(root.transform,false);
                    if(type==typeof(ParticleSystemRenderer))child.AddComponent<ParticleSystem>();
                    var renderer=type==typeof(ParticleSystemRenderer)?child.GetComponent<ParticleSystemRenderer>():child.AddComponent(type) as Renderer;
                    if(renderer==null)throw new InvalidOperationException("Property fixture renderer is missing");
                    var input=new[]{RustRendererFilter.CachedPointer(renderer),RustRendererFilter.CachedPointer(renderer)};
                    foreach(var color in new[]{new Color(0,0,0,0),new Color(0.123f,0.456f,0.789f,1),new Color(-1,2,0.5f,0.25f)})
                    {
                        reference.Clear();reference.SetFloat(marker,3.75f);reference.SetVector(vectorMarker,vector);reference.SetMatrix(matrixMarker,matrix);reference.SetTexture(textureMarker,Texture2D.whiteTexture);reference.SetColor(exposedColor,color);renderer.SetPropertyBlock(reference);
                        renderer.GetPropertyBlock(read);Color expected=read.GetColor(exposedColor);
                        block.Clear();block.SetFloat(marker,3.75f);block.SetVector(vectorMarker,vector);block.SetMatrix(matrixMarker,matrix);block.SetTexture(textureMarker,Texture2D.whiteTexture);Color value=color;
                        int result;
                        fixed(IntPtr* pointers=input)result=kernel((IntPtr)pointers,(UIntPtr)(uint)input.Length,blockPointer(block),exposedColor,ref value,ref api);
                        renderer.GetPropertyBlock(read);
                        var actual=read.GetColor(exposedColor);
                        if(result!=0||!SameBits(actual,expected)||!SameBits(value,color)||read.GetFloat(marker)!=3.75f||read.GetVector(vectorMarker)!=vector||read.GetMatrix(matrixMarker)!=matrix||read.GetTexture(textureMarker)!=Texture2D.whiteTexture)throw new InvalidOperationException("Native property fixture differs");
                        Fixtures++;
                    }
                }
            }
            finally{UnityEngine.Object.DestroyImmediate(root);}
        }
        static bool SameBits(Color a,Color b)
        {
            for(int i=0;i<4;i++)if(BitConverter.ToInt32(BitConverter.GetBytes(a[i]),0)!=BitConverter.ToInt32(BitConverter.GetBytes(b[i]),0))return false;
            return true;
        }
    }
}
