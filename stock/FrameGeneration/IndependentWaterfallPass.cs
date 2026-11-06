// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class IndependentWaterfallPass : IDisposable
    {
        struct Draw
        {
            public Renderer Renderer;
            public Material Material;
            public int Queue;
            public float Distance;
        }
        static readonly HashSet<string> supported=new HashSet<string>(StringComparer.Ordinal)
        {
            "Waterfall/Additive Directional","Waterfall/Billboard (Additive)","Waterfall/Additive (Dynamic)",
            "Waterfall/Additive Echo (Dynamic)","Waterfall/Alpha (Dynamic)","Waterfall/Procedural Particles (Additive)",
            "Waterfall/Additive Detail (Dynamic)","Waterfall/Additive Cones (Volumetric)",
            "Waterfall/Billboard (Additive Directional)","KSP/Particles/Additive","Waterfall/Additive (Volumetric)",
            "Waterfall/Additive","Waterfall/Alpha Directional"
        };
        Type type;
        FieldInfo rendererField;
        bool resolved;
        float refresh;
        UnityEngine.Object[] modules=new UnityEngine.Object[0];
        readonly HashSet<Renderer> seen=new HashSet<Renderer>();
        readonly List<Draw> draws=new List<Draw>();
        Camera camera;
        CommandBuffer commands;
        static int Order(Draw left,Draw right)
        {int queue=left.Queue.CompareTo(right.Queue);return queue!=0?queue:right.Distance.CompareTo(left.Distance);}
        public void Prepare(Camera current,IntPtr renderEvent)
        {
            if(!HighLogic.LoadedSceneIsFlight || FlightCamera.fetch==null || current!=FlightCamera.fetch.mainCamera)return;
            if(!resolved)
            {
                resolved=true;
                foreach(Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                    if((type=assembly.GetType("Waterfall.ModuleWaterfallFX",false))!=null)break;
                if(type!=null)rendererField=type.GetField("allRenderers",BindingFlags.Instance|BindingFlags.NonPublic);
            }
            if(type==null || rendererField==null)return;
            if(camera!=current)
            {
                Dispose();camera=current;commands=new CommandBuffer{name="KOE isolated Waterfall emission"};
                camera.AddCommandBuffer(CameraEvent.AfterForwardAlpha,commands);
            }
            if(Time.realtimeSinceStartup>=refresh)
            {modules=UnityEngine.Object.FindObjectsOfType(type);refresh=Time.realtimeSinceStartup+.5f;}
            draws.Clear();seen.Clear();Vector3 position=current.transform.position;
            foreach(UnityEngine.Object module in modules)
            {
                if(module==null)continue;
                var renderers=rendererField.GetValue(module) as Renderer[];
                if(renderers==null)continue;
                foreach(Renderer renderer in renderers)
                {
                    if(renderer==null || !renderer.enabled || !renderer.gameObject.activeInHierarchy || !seen.Add(renderer)
                        || (current.cullingMask&(1<<renderer.gameObject.layer))==0)continue;
                    Material material=renderer.sharedMaterial;
                    if(material==null || material.shader==null || !supported.Contains(material.shader.name))continue;
                    draws.Add(new Draw{Renderer=renderer,Material=material,Queue=material.renderQueue,Distance=(renderer.bounds.center-position).sqrMagnitude});
                }
            }
            draws.Sort(Order);commands.Clear();commands.IssuePluginEvent(renderEvent,25);
            foreach(Draw draw in draws)commands.DrawRenderer(draw.Renderer,draw.Material,0,0);
            commands.IssuePluginEvent(renderEvent,27);
        }
        public void Dispose()
        {
            if(commands!=null)
            {
                if(camera!=null)camera.RemoveCommandBuffer(CameraEvent.AfterForwardAlpha,commands);
                commands.Dispose();commands=null;
            }
            camera=null;modules=new UnityEngine.Object[0];draws.Clear();seen.Clear();refresh=0;
        }
    }
}
