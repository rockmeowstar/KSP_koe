"""Flush a deferred producer before any GPU operation that can observe or change it."""
import argparse,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
def strip_annotations(text):
    text=re.sub(r'/\*.*?\*/',' ',text,flags=re.S)
    result='';i=0
    while i<len(text):
        match=re.match(r'_[A-Za-z0-9_]+_',text[i:]) if i==0 or not(text[i-1].isalnum() or text[i-1]=='_') else None
        if not match:result+=text[i];i+=1;continue
        i+=len(match[0])
        while i<len(text) and text[i].isspace():i+=1
        if i<len(text) and text[i]=='(':
            level=1;i+=1
            while level:
                if text[i]=='(':level+=1
                if text[i]==')':level-=1
                i+=1
    return re.sub(r'\s+',' ',result).strip()
t=(a.sdk/'d3d11_3.h').read_text();t=t[t.index('typedef struct ID3D11DeviceContext4Vtbl'):];t=t[:t.index('} ID3D11DeviceContext4Vtbl;')]
methods=[]
for ret,name,params in re.findall(r'(\w+)\s*\(\s*STDMETHODCALLTYPE\s*\*(\w+)\s*\)\s*\((.*?)\);',t,re.S):
 params=strip_annotations(params);names=[re.search(r'(\w+)\s*(?:\[\s*\d*\s*\])?\s*$',v.strip())[1] for v in params.split(',')];methods.append((ret,name,params,names))
if len(methods)!=149 or methods[12][1]!='DrawIndexed':raise RuntimeError('Unknown SDK layout')
lines=['// Generated from SDK.'];hooks=[]
for slot,(ret,name,params,names) in enumerate(methods):
 if name in ('PSSetShader','SetMarkerInt','BeginEventInt','EndEvent','IsAnnotationEnabled'):continue
 if slot<7 or name.startswith('Get') and name!='GetData' or re.match(r'^(VS|PS|GS|HS|DS|CS|IA|OM|RS|SO)Get',name):continue
 safe=re.match(r'^(VS|PS|GS|HS|DS|CS|IA|OM|RS)Set',name) and name not in ('CSSetUnorderedAccessViews','OMSetRenderTargetsAndUnorderedAccessViews')
 if safe and name!='PSSetShader':continue
 hooks.append((slot,name));lines.append(f'using Fn_{name}={ret}(STDMETHODCALLTYPE*)({params}); static Fn_{name} original_{name}{{}};')
 if name in ('DrawIndexed','PSSetShader'):lines.append(f'static {ret} STDMETHODCALLTYPE Hook_{name}({params});');continue
 resources=[n for n in names if n in ('pResource','pDstResource','pSrcResource','pDstBuffer')]
 views=[n for n in names if n in ('pRenderTargetView','pDepthStencilView','pUnorderedAccessView','pResourceView','pShaderResourceView')]
 body=f'if(!inside) BeforeBoundary(This,{slot});'
 if resources:body='if(!inside) ResourceBoundary(This,{'+','.join(resources)+'},'+str(slot)+');'
 elif len(views)==1:body='if(!inside) ViewBoundary(This,'+views[0]+','+str(slot)+');'
 if name in ('Begin','End'):body=f'if(!inside&&!IsKoeTimingProbeQuery(pAsync)) BeforeBoundary(This,{slot});'
 if name=='Begin':body+='if(!inside) BeginCloudQuery(This,pAsync);'
 if name=='End':body+='if(!inside) EndCloudQuery(This,pAsync);'
 if name in ('ClearState','ExecuteCommandList','SwapDeviceContextState'):body+='if(!inside) boundKind=-1;'
 lines.append(f'static {ret} STDMETHODCALLTYPE Hook_{name}({params}){{{body}{"return " if ret!="void" else ""}original_{name}({",".join(names)});}}')
lines.append('static HookSpec contextHooks[]={'+','.join('{%d,reinterpret_cast<void*>(Hook_%s),reinterpret_cast<void**>(&original_%s)}'%(slot,name,name) for slot,name in hooks)+'};')
a.output.write_text('\n'.join(lines)+'\n');print('Cloud fusion GPU boundaries:',len(hooks))
