// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace KoeFrameGeneration
{
    sealed class AtmosphereSourceProbe
    {
        Type type;
        FieldInfo color,extinction,depth,material;
        Component source;
        Camera camera;
        bool resolved;
        readonly Dictionary<UnityEngine.Texture,IntPtr> handles=new Dictionary<UnityEngine.Texture,IntPtr>();
        readonly float[] z=new float[4];
        [System.Runtime.InteropServices.UnmanagedFunctionPointer(System.Runtime.InteropServices.CallingConvention.Cdecl)]
        public delegate void Send(IntPtr color,IntPtr extinction,IntPtr depth,int tone,[System.Runtime.InteropServices.In] float[] z);
        IntPtr Handle(UnityEngine.Texture value)
        {
            if(value==null)return IntPtr.Zero;
            IntPtr handle;
            if(!handles.TryGetValue(value,out handle))handles[value]=handle=value.GetNativeTexturePtr();
            return handle;
        }
        public void Observe(Camera current,Send send)
        {
            if(!HighLogic.LoadedSceneIsFlight || FlightCamera.fetch==null || current!=FlightCamera.fetch.mainCamera)return;
            if(!resolved)
            {
                resolved=true;
                foreach(Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if((type=assembly.GetType("Scatterer.ScatteringCommandBuffer",false))!=null)break;
                if(type==null)return;
                const BindingFlags flags=BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic;
                color=type.GetField("downscaledRenderTexture0",flags);
                extinction=type.GetField("downscaledRenderTexture1",flags);
                depth=type.GetField("downscaledDepthRenderTexture",flags);
                material=type.GetField("targetMaterial",flags);
            }
            if(type==null || color==null || extinction==null || depth==null)return;
            if(camera!=current || source==null){camera=current;source=current.GetComponent(type);}
            if(source==null)return;
            var c=color.GetValue(source) as UnityEngine.Texture;
            var t=extinction.GetValue(source) as UnityEngine.Texture;
            var d=depth.GetValue(source) as UnityEngine.Texture;
            if(c==null || t==null || d==null)return;
            var shader=material==null?null:material.GetValue(source) as Material;
            Vector4 value=Shader.GetGlobalVector("_ZBufferParams");z[0]=value.x;z[1]=value.y;z[2]=value.z;z[3]=value.w;
            int tone=shader!=null&&shader.HasProperty("TONEMAPPING_MODE")?shader.GetInt("TONEMAPPING_MODE"):Shader.GetGlobalInt("TONEMAPPING_MODE");
            send(Handle(c),Handle(t),Handle(d),tone,z);
        }
        public void Clear(){source=null;camera=null;handles.Clear();}
    }
}
