using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using HarmonyLib;
using UnityEngine;

namespace KoeFarEventPath
{
    internal static class RustRendererFilter
    {
        const string Id = "KerbalOverEngineering.FAR.RustRendererFilter";
        [DllImport("kernel32", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module,string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int Kernel(IntPtr pointers,UIntPtr count,IntPtr flags,UIntPtr ownerOffset,UIntPtr layerOffset);
        static Kernel kernel;
        static IntPtr library;
        static Func<Renderer,IntPtr> pointer;
        static MethodInfo expectedPredicate;
        static MethodInfo[] watched;
        [ThreadStatic] static bool allowed;
        internal static uint OwnerOffset, LayerOffset;
        static bool audit,failed;
        static int mainThread;
        public static long Calls, Items, Fallbacks, Audits, Mismatches, Fixtures;
        [ThreadStatic] static Stack<Batch> available;
        sealed class Batch
        {
            internal IntPtr[] Pointers=new IntPtr[0];
            internal byte[] Flags=new byte[0];
            internal int Cursor;
            internal readonly Predicate<Renderer> Selected;
            internal Batch(){Selected=_=>Flags[Cursor++]!=0;}
            internal void Ensure(int count)
            {
                if(Pointers.Length>=count)return;
                int capacity=Math.Max(count,Math.Max(1,Pointers.Length*2));
                Pointers=new IntPtr[capacity];Flags=new byte[capacity];
            }
        }

        internal static void Apply()
        {
            string path=Environment.GetEnvironmentVariable("KOE_FAR_RUST_FILTER");
            if(string.IsNullOrEmpty(path))
            {
                if(!KerbalOverEngineering.Options.Settings.Enabled("FAR.RustRendererFilter"))return;
                try{path=Path.Combine(KerbalOverEngineering.Options.NativeAssets.Extract(Assembly.GetExecutingAssembly()),"KoeFarRendererFilter.dll");}
                catch(Exception error){Debug.LogWarning("[KOE FAR RustFilter] Native payload unavailable: "+error.Message);return;}
            }
            var harmony=new Harmony(Id);
            try
            {
                mainThread=Thread.CurrentThread.ManagedThreadId;
                if(IntPtr.Size!=8)throw new NotSupportedException("Reviewed engine binding requires x64");
                var type=AccessTools.TypeByName("FerramAerospaceResearch.FARAeroComponents.VehicleExposure");
                if(Hash(File.ReadAllBytes(type.Assembly.Location))!="cef968a7378b8a5a31fd3cfa3e5cf72fdca8c46248f8fe4280e56b42d8277fd5")
                    throw new NotSupportedException("FAR filter implementation has not been reviewed");
                var method=AccessTools.DeclaredMethod(type,"ResetParts").MakeGenericMethod(typeof(List<Part>));
                foreach(var instruction in PatchProcessor.GetOriginalInstructions(method))
                    if(instruction.opcode==OpCodes.Ldftn)expectedPredicate=instruction.operand as MethodInfo;
                if(expectedPredicate==null)throw new NotSupportedException("FAR filter predicate is missing");
                watched=new[]{expectedPredicate,AccessTools.Method(typeof(UnityEngine.Object),"op_Inequality"),AccessTools.PropertyGetter(typeof(Component),"gameObject"),AccessTools.PropertyGetter(typeof(GameObject),"layer")};
                if(!Compatible())throw new NotSupportedException("Renderer filtering API is already patched");
                var patches=Harmony.GetPatchInfo(method);
                if(patches!=null)foreach(var owner in patches.Owners)
                    if(owner!="KerbalOverEngineering.FAR.RendererInitDispatch")throw new NotSupportedException("FAR reset already patched by "+owner);
                byte[] engine=ReadEngine();
                OwnerOffset=ReadFieldOffset(engine,0x926aac,new byte[]{0x48,0x8b,0x52});
                LayerOffset=ReadFieldOffset(engine,0x93dacc,new byte[]{0x8b,0x41});
                LoadKernel(path);
                audit=Environment.GetEnvironmentVariable("KOE_FAR_RUST_AUDIT")=="1";
                CheckFixtures();
                harmony.Patch(method,new HarmonyMethod(typeof(RustRendererFilter),"Before"),null,new HarmonyMethod(typeof(RustRendererFilter),"Rewrite"),new HarmonyMethod(typeof(RustRendererFilter),"After"));
                Application.quitting+=Report;
                Debug.Log("[KOE FAR RustFilter] Enabled; fixtureCases="+Fixtures+"; audit="+audit);
            }
            catch(Exception error)
            {
                harmony.UnpatchAll(Id);failed=true;
                Debug.LogError("[KOE FAR RustFilter] Disabled: "+error);
            }
        }

        static bool Compatible()
        {
            if(failed||watched==null)return false;
            foreach(var method in watched)if(method==null||Harmony.GetPatchInfo(method)!=null)return false;
            return true;
        }
        static void Before(out bool __state){__state=allowed;allowed=Thread.CurrentThread.ManagedThreadId==mainThread&&Compatible();}
        static Exception After(bool __state,Exception __exception){allowed=__state;return __exception;}

        internal static string Hash(byte[] bytes)
        {
            using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-","").ToLowerInvariant();
        }
        internal static byte[] ReadEngine()
        {
            byte[] engine=File.ReadAllBytes(Path.Combine(KSPUtil.ApplicationRootPath,"UnityPlayer.dll"));
            if(Hash(engine)!="e661981f033e2f1d32db6e44d43c0e0c1783b4cb3115caf393e7722673473317")
                throw new NotSupportedException("Unity layout has not been reviewed");
            return engine;
        }
        internal static IntPtr CachedPointer(Renderer renderer){return ReferenceEquals(renderer,null)?IntPtr.Zero:pointer(renderer);}
        internal static IntPtr Entry(string name){return GetProcAddress(library,name);}
        static uint ReadFieldOffset(byte[] image,int rva,byte[] prefix)
        {
            int pe=BitConverter.ToInt32(image,60),count=BitConverter.ToUInt16(image,pe+6);
            int section=pe+24+BitConverter.ToUInt16(image,pe+20);
            for(int i=0;i<count;i++,section+=40)
            {
                int start=BitConverter.ToInt32(image,section+12),size=BitConverter.ToInt32(image,section+16);
                if(rva<start||rva>=start+size)continue;
                int offset=BitConverter.ToInt32(image,section+20)+rva-start;
                for(int b=0;b<prefix.Length;b++)if(image[offset+b]!=prefix[b])throw new NotSupportedException("Unity field instruction differs");
                return image[offset+prefix.Length];
            }
            throw new NotSupportedException("Unity field instruction is outside image sections");
        }
        internal static void LoadKernel(string path)
        {
            var field=AccessTools.Field(typeof(UnityEngine.Object),"m_CachedPtr");
            if(field==null||field.FieldType!=typeof(IntPtr))throw new NotSupportedException("Unity native pointer field differs");
            var read=new DynamicMethod("KOE_RendererPointer",typeof(IntPtr),new[]{typeof(Renderer)},typeof(RustRendererFilter),true);
            var il=read.GetILGenerator();il.Emit(OpCodes.Ldarg_0);il.Emit(OpCodes.Ldfld,field);il.Emit(OpCodes.Ret);
            pointer=(Func<Renderer,IntPtr>)read.CreateDelegate(typeof(Func<Renderer,IntPtr>));
            IntPtr module=LoadLibraryW(Path.GetFullPath(path));
            if(module==IntPtr.Zero)throw new InvalidOperationException("Rust filter library could not be loaded");
            library=module;
            IntPtr entry=GetProcAddress(module,"KoeFilterRendererLayers");
            if(entry==IntPtr.Zero)throw new NotSupportedException("Rust filter export is missing");
            kernel=(Kernel)Marshal.GetDelegateForFunctionPointer(entry,typeof(Kernel));
        }
        internal static IEnumerable<CodeInstruction> Rewrite(IEnumerable<CodeInstruction> code)
        {
            int count=0;var target=AccessTools.Method(typeof(List<Renderer>),"RemoveAll");
            foreach(var instruction in code)
            {
                var copy=new CodeInstruction(instruction);
                if(copy.Calls(target)){copy.opcode=OpCodes.Call;copy.operand=AccessTools.Method(typeof(RustRendererFilter),"Remove");count++;}
                yield return copy;
            }
            if(count!=1)throw new NotSupportedException("Expected one FAR renderer filter");
        }
        public static int Remove(List<Renderer> list,Predicate<Renderer> predicate)
        {
            if(failed||!allowed||predicate==null||!Equals(predicate.Method,expectedPredicate))return list.RemoveAll(predicate);
            return Filter(list,predicate);
        }
        internal static unsafe int Filter(List<Renderer> list,Predicate<Renderer> original)
        {
            int count=list.Count;
            if(count==0)return 0;
            if(available==null)available=new Stack<Batch>();
            var batch=available.Count==0?new Batch():available.Pop();
            try
            {
                batch.Ensure(count);batch.Cursor=0;
                for(int i=0;i<count;i++)batch.Pointers[i]=ReferenceEquals(list[i],null)?IntPtr.Zero:pointer(list[i]);
                int result;
                fixed(IntPtr* input=batch.Pointers)fixed(byte* output=batch.Flags)
                    result=kernel((IntPtr)input,(UIntPtr)(uint)count,(IntPtr)output,(UIntPtr)OwnerOffset,(UIntPtr)LayerOffset);
                if(result!=0){Fallbacks++;return list.RemoveAll(original);}
                for(int i=0;i<count;i++)
                {
                    if(batch.Pointers[i]==IntPtr.Zero)batch.Flags[i]=(byte)(original(list[i])?1:0);
                    if(audit){Audits++;if((batch.Flags[i]!=0)!=original(list[i])){Mismatches++;failed=true;Fallbacks++;return list.RemoveAll(original);}}
                }
                Calls++;Items+=count;
                return list.RemoveAll(batch.Selected);
            }
            finally{available.Push(batch);GC.KeepAlive(list);}
        }
        static void CheckFixtures()
        {
            var root=new GameObject("KOE owned renderer filter fixture");root.hideFlags=HideFlags.HideAndDontSave;
            try
            {
                var source=new List<Renderer>();
                foreach(var type in new[]{typeof(MeshRenderer),typeof(SkinnedMeshRenderer),typeof(LineRenderer),typeof(TrailRenderer)})
                {
                    var child=new GameObject("KOE owned "+type.Name);child.transform.SetParent(root.transform,false);
                    var renderer=child.AddComponent(type) as Renderer;
                    if(renderer==null)throw new InvalidOperationException("Renderer fixture component could not be created");
                    source.Add(renderer);
                }
                source.Add(null);
                Predicate<Renderer> original=r=>r!=null&&r.gameObject.layer!=0;
                for(int layer=0;layer<32;layer++)foreach(bool active in new[]{false,true})
                {
                    root.layer=layer;root.SetActive(active);
                    foreach(var renderer in source)if(renderer!=null)renderer.gameObject.layer=layer;
                    var expected=new List<Renderer>(source);var actual=new List<Renderer>(source);
                    int removed=expected.RemoveAll(original),found=Filter(actual,original);
                    if(found!=removed||actual.Count!=expected.Count)throw new InvalidOperationException("Rust fixture count differs");
                    for(int i=0;i<actual.Count;i++)if(!ReferenceEquals(actual[i],expected[i]))throw new InvalidOperationException("Rust fixture renderer order differs");
                    Fixtures++;
                }
                UnityEngine.Object.DestroyImmediate(source[0]);
                var destroyed=new List<Renderer>(source);var check=new List<Renderer>(source);
                destroyed.RemoveAll(original);Filter(check,original);
                if(destroyed.Count!=check.Count)throw new InvalidOperationException("Destroyed renderer fallback differs");
                for(int i=0;i<check.Count;i++)if(!ReferenceEquals(check[i],destroyed[i]))throw new InvalidOperationException("Destroyed renderer identity differs");
                Fixtures++;
            }
            finally{UnityEngine.Object.DestroyImmediate(root);}
        }
        static void Report()
        {
            Debug.Log("[KOE FAR RustFilter] calls="+Calls+"; items="+Items+"; audits="+Audits+"; mismatches="+Mismatches+"; fallbacks="+Fallbacks+"; failed="+failed);
        }
    }
}
