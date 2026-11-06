// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace KoeFrameGeneration
{
    sealed class IndependentEffectPass : IDisposable
    {
        sealed class Pass
        {
            public Camera Camera;
            public CameraEvent Stage;
            public CommandBuffer Begin,End;
            public CommandBuffer[] Layout;
        }
        readonly Dictionary<Camera,Pass> passes=new Dictionary<Camera,Pass>();
        static bool Firefly(CommandBuffer buffer)
        { return buffer.sizeInBytes>0 && buffer.name.StartsWith("Firefly atmospheric effects [",StringComparison.Ordinal); }
        static List<CommandBuffer> Canonical(Pass pass,CommandBuffer[] buffers)
        {
            var result=new List<CommandBuffer>();var seen=new HashSet<CommandBuffer>();
            foreach(CommandBuffer buffer in buffers)
            {
                if(buffer==pass.Begin||buffer==pass.End)continue;
                if(Firefly(buffer)&&!seen.Add(buffer))continue;
                result.Add(buffer);
            }
            return result;
        }
        public void Prepare(Camera camera,IntPtr renderEvent)
        {
            if(camera==null || camera!= (FlightCamera.fetch==null?null:FlightCamera.fetch.mainCamera))return;
            Pass pass;
            if(!passes.TryGetValue(camera,out pass))
            {
                CameraEvent stage=CameraEvent.AfterForwardAlpha;bool found=false;
                foreach(CameraEvent candidate in Enum.GetValues(typeof(CameraEvent)))
                    foreach(CommandBuffer buffer in camera.GetCommandBuffers(candidate))
                        if(Firefly(buffer)){stage=candidate;found=true;}
                if(!found)return;
                pass=new Pass{Camera=camera,Stage=stage};passes.Add(camera,pass);
            }
            CommandBuffer[] current=camera.GetCommandBuffers(pass.Stage);
            bool unchanged=pass.Layout!=null && current.Length==pass.Layout.Length;
            if(unchanged)for(int i=0;i<current.Length;i++)if(current[i]!=pass.Layout[i]){unchanged=false;break;}
            if(unchanged)return;
            List<CommandBuffer> original=Canonical(pass,current);
            int first=original.FindIndex(Firefly),last=original.FindLastIndex(Firefly);
            if(first<0)return;
            for(int i=first;i<=last;i++)if(!Firefly(original[i]))return;
            if(pass.Begin==null)
            {
                pass.Begin=new CommandBuffer{name="KOE isolated Firefly begin"};
                pass.End=new CommandBuffer{name="KOE isolated Firefly end"};
            }
            pass.Begin.Clear();pass.End.Clear();
            // AI-generated comment: Native capture saves and restores the exact active HDR RTV/DSV.
            // BuiltinRenderTextureType.CameraTarget can identify a different target in this pipeline.
            pass.Begin.IssuePluginEvent(renderEvent,13);
            pass.End.IssuePluginEvent(renderEvent,15);
            var layout=new List<CommandBuffer>();
            for(int i=0;i<original.Count;i++)
            {
                if(i==first)
                {
                    layout.Add(pass.Begin);
                    for(int j=first;j<=last;j++)layout.Add(original[j]);
                    layout.Add(pass.End);
                }
                layout.Add(original[i]);
            }
            foreach(CommandBuffer buffer in current)camera.RemoveCommandBuffer(pass.Stage,buffer);
            foreach(CommandBuffer buffer in layout)camera.AddCommandBuffer(pass.Stage,buffer);
            pass.Layout=layout.ToArray();
        }
        public void Dispose()
        {
            foreach(Pass pass in passes.Values)
            {
                if(pass.Camera!=null)
                {
                    var buffers=pass.Camera.GetCommandBuffers(pass.Stage);
                    var original=Canonical(pass,buffers);
                    foreach(CommandBuffer buffer in buffers)pass.Camera.RemoveCommandBuffer(pass.Stage,buffer);
                    foreach(CommandBuffer buffer in original)pass.Camera.AddCommandBuffer(pass.Stage,buffer);
                }
                if(pass.Begin!=null)pass.Begin.Dispose();if(pass.End!=null)pass.End.Dispose();
            }
            passes.Clear();
        }
    }
}
