// SPDX-License-Identifier: MIT
using System;
using System.Reflection;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class IndependentSkyPass : IDisposable
    {
        Type type;
        FieldInfo rendererField,materialField;
        PropertyInfo instance;
        FieldInfo near,unified;
        bool resolved;
        float refresh;
        UnityEngine.Object[] skies=new UnityEngine.Object[0];
        readonly Dictionary<Camera,CommandBuffer> passes=new Dictionary<Camera,CommandBuffer>();
        public void Prepare(Camera current,IntPtr renderEvent)
        {
            if(!HighLogic.LoadedSceneIsFlight || current==null || current.orthographic || current.targetTexture!=null
                || current.cameraType!=CameraType.Game || current.pixelWidth!=Screen.width || current.pixelHeight!=Screen.height)return;
            if(!resolved)
            {
                resolved=true;
                foreach(Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if((type=assembly.GetType("Scatterer.SkySphereLocalRenderer",false))!=null)break;
                if(type!=null)
                {
                    const BindingFlags flags=BindingFlags.Instance|BindingFlags.NonPublic;
                    rendererField=type.GetField("renderer",flags);materialField=type.GetField("material",flags);
                    Type controller=type.Assembly.GetType("Scatterer.Scatterer",false);
                    if(controller!=null)
                    {
                        instance=controller.GetProperty("Instance",BindingFlags.Static|BindingFlags.Public);
                        near=controller.GetField("nearCamera",BindingFlags.Instance|BindingFlags.Public);
                        unified=controller.GetField("unifiedCameraMode",BindingFlags.Instance|BindingFlags.Public);
                    }
                }
            }
            if(type==null || rendererField==null || materialField==null || instance==null || near==null || unified==null)return;
            object controllerInstance=instance.GetValue(null,null);
            if(controllerInstance==null || (!(bool)unified.GetValue(controllerInstance) && near.GetValue(controllerInstance) as Camera==current))return;
            CommandBuffer commands;
            if(!passes.TryGetValue(current,out commands))
            {
                commands=new CommandBuffer{name="KOE isolated atmospheric sky"};passes.Add(current,commands);
                current.AddCommandBuffer(CameraEvent.BeforeForwardAlpha,commands);
            }
            if(Time.realtimeSinceStartup>=refresh)
            {skies=UnityEngine.Object.FindObjectsOfType(type);refresh=Time.realtimeSinceStartup+.5f;}
            commands.Clear();bool began=false;
            foreach(UnityEngine.Object sky in skies)
            {
                if(sky==null)continue;
                var renderer=rendererField.GetValue(sky) as Renderer;
                var material=materialField.GetValue(sky) as Material;
                if(renderer!=null && renderer.enabled && renderer.gameObject.activeInHierarchy && material!=null
                    && (current.cullingMask&(1<<renderer.gameObject.layer))!=0)
                {
                    if(!began){commands.IssuePluginEvent(renderEvent,21);began=true;}
                    commands.DrawRenderer(renderer,material,0,2);
                }
            }
            if(began)commands.IssuePluginEvent(renderEvent,23);
        }
        public void Dispose()
        {
            foreach(var pass in passes)
            {
                if(pass.Key!=null)pass.Key.RemoveCommandBuffer(CameraEvent.BeforeForwardAlpha,pass.Value);
                pass.Value.Dispose();
            }
            passes.Clear();skies=new UnityEngine.Object[0];refresh=0;
        }
    }
}
