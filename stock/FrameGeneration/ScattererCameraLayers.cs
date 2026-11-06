// SPDX-License-Identifier: MIT
using System;
using System.Reflection;
using UnityEngine;

namespace KoeFrameGeneration
{
    sealed class ScattererCameraLayers
    {
        bool resolved;
        PropertyInfo instance;
        FieldInfo near,far,unified;
        public bool OwnSky(Camera camera)
        {
            if(!resolved)
            {
                resolved=true;Type type=null;
                foreach(Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if((type=assembly.GetType("Scatterer.Scatterer",false))!=null)break;
                if(type!=null)
                {
                    instance=type.GetProperty("Instance",BindingFlags.Static|BindingFlags.Public);
                    near=type.GetField("nearCamera",BindingFlags.Instance|BindingFlags.Public);
                    far=type.GetField("farCamera",BindingFlags.Instance|BindingFlags.Public);
                    unified=type.GetField("unifiedCameraMode",BindingFlags.Instance|BindingFlags.Public);
                }
            }
            if(instance==null || near==null || far==null || unified==null)return false;
            object current=instance.GetValue(null,null);
            return current!=null && camera==(((bool)unified.GetValue(current)?near:far).GetValue(current) as Camera);
        }
    }
}
