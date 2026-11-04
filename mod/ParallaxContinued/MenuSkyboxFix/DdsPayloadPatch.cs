using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using HarmonyLib;
using UnityEngine;

namespace MenuSkyboxFix
{
    public static class DdsPayloadPatch
    {
        public sealed class Scope {public Scope Parent;public byte[] Buffer;}
        [ThreadStatic] static Scope current;
        public static long AvoidedArrays,AvoidedCopyBytes,PointerUploads;
        public static byte[] CurrentBuffer {get{return current==null?null:current.Buffer;}}
        public static MethodInfo Bind(Assembly assembly)
        {
            if(typeof(ModuleEngines).Assembly.ManifestModule.ModuleVersionId!=new Guid("4b449f28-41f8-4227-adfa-ad3149c8fdba")||assembly.ManifestModule.ModuleVersionId!=new Guid("1ecb998e-ce0b-4bfd-970f-fe0fa2d99def"))throw new NotSupportedException("Unknown KSP/Parallax DDS loader");
            var method=AccessTools.DeclaredMethod(assembly.GetType("Parallax.TextureLoader",true),"LoadDDSTexture",new[]{typeof(string),typeof(bool),typeof(bool)});
            if(method==null||!method.IsStatic||method.ReturnType!=typeof(Texture2D))throw new NotSupportedException("Parallax DDS signature changed");return method;
        }
        public static void Apply()
        {
            const string id="local.kerbaloverengineering.parallax-dds-payload";var h=new Harmony(id);
            var target=Bind(AppDomain.CurrentDomain.GetAssemblies().Single(a=>a.GetName().Name=="ParallaxContinued"));
            var info=Harmony.GetPatchInfo(target);if(info!=null&&info.Owners.Contains(id))return;
            try{h.Patch(target,new HarmonyMethod(typeof(DdsPayloadPatch),"Begin"),null,new HarmonyMethod(typeof(DdsPayloadPatch),"Transpile"),new HarmonyMethod(typeof(DdsPayloadPatch),"End"));}
            catch{h.UnpatchAll(id);throw;}
        }
        static void Begin(out Scope __state){__state=new Scope{Parent=current};current=__state;}
        static Exception End(Exception __exception,Scope __state){if(__state!=null)current=__state.Parent;return __exception;}
        public static byte[] Read(string path)
        {var bytes=File.ReadAllBytes(path);if(current!=null)current.Buffer=bytes;return bytes;}
        public static byte[] Borrow(int count)
        {var bytes=CurrentBuffer;return bytes!=null&&bytes.Length-128==count?bytes:new byte[count];}
        public static void Copy(Array source,int sourceOffset,Array target,int targetOffset,int count)
        {
            if(ReferenceEquals(source,target)&&sourceOffset==128&&targetOffset==0&&count==source.Length-128)
            {AvoidedArrays++;AvoidedCopyBytes+=count;return;}
            Buffer.BlockCopy(source,sourceOffset,target,targetOffset,count);
        }
        public static Texture2D Upload<T>(Texture2D texture,byte[] bytes,ref T data) where T:struct
        {
            try
            {
                int offset=ReferenceEquals(bytes,CurrentBuffer)?128:0;int count=bytes.Length-offset;
                // Original NativeArray.CopyFrom requires an exact byte count too; preserve that error path.
                if(texture.GetRawTextureData<byte>().Length!=count)throw new ArgumentException("source and destination length must be the same");
                var pin=GCHandle.Alloc(bytes,GCHandleType.Pinned);
                try{texture.LoadRawTextureData(IntPtr.Add(pin.AddrOfPinnedObject(),offset),count);PointerUploads++;}
                finally{pin.Free();}
                return texture;
            }
            catch(Exception ex){Debug.LogError("[Parallax] Error loading texture data: "+ex.Message);return null;}
        }
        public static IEnumerable<CodeInstruction> Transpile(IEnumerable<CodeInstruction> input)
        {
            var code=input.Select(c=>new CodeInstruction(c)).ToList();int files=0,arrays=0,copies=0,uploads=0;
            foreach(var c in code)
            {
                if(c.Calls(typeof(File).GetMethod("ReadAllBytes",new[]{typeof(string)})))
                {c.operand=AccessTools.Method(typeof(DdsPayloadPatch),"Read");files++;}
                if(c.opcode==OpCodes.Newarr&&Equals(c.operand,typeof(byte)))
                {c.opcode=OpCodes.Call;c.operand=AccessTools.Method(typeof(DdsPayloadPatch),"Borrow");arrays++;}
                if(c.Calls(typeof(Buffer).GetMethod("BlockCopy",new[]{typeof(Array),typeof(int),typeof(Array),typeof(int),typeof(int)})))
                {c.operand=AccessTools.Method(typeof(DdsPayloadPatch),"Copy");copies++;}
                var method=c.operand as MethodInfo;
                if(method==null||method.DeclaringType==null||method.DeclaringType.FullName!="Parallax.TextureLoader"||method.Name!="Texture2DFromData")continue;
                var parameters=method.GetParameters();
                if(parameters.Length!=3||parameters[0].ParameterType!=typeof(Texture2D)||parameters[1].ParameterType!=typeof(byte[])||!parameters[2].ParameterType.IsByRef)throw new NotSupportedException("Parallax upload helper changed");
                c.opcode=OpCodes.Call;c.operand=typeof(DdsPayloadPatch).GetMethod("Upload").MakeGenericMethod(parameters[2].ParameterType.GetElementType());uploads++;
            }
            if(files!=1||arrays!=1||copies!=1||uploads!=1)throw new NotSupportedException("Parallax DDS layout changed");return code;
        }
    }
}
