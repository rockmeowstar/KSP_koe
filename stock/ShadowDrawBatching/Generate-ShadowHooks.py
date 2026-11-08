"""Generate typed D3D11 boundaries from the installed SDK for the replay prototype."""
import argparse
from pathlib import Path
import re

p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
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
s=(a.sdk/'d3d11_3.h').read_text();start=s.index('typedef struct ID3D11DeviceContext4Vtbl');s=s[start:s.index('} ID3D11DeviceContext4Vtbl;',start)]
methods=[]
for ret,name,params in re.findall(r'(\w+)\s*\(\s*STDMETHODCALLTYPE\s*\*(\w+)\s*\)\s*\((.*?)\);',s,re.S):
    params=strip_annotations(params)
    names=[re.search(r'(\w+)\s*(?:\[\s*\d*\s*\])?\s*$',v.strip())[1] for v in params.split(',')]
    methods.append((ret,name,params,names))
if len(methods)!=149 or methods[12][1]!='DrawIndexed':raise RuntimeError('SDK vtable changed')
special={'DrawIndexed','Map','Unmap','UpdateSubresource','UpdateSubresource1','CopyResource',
         'CopySubresourceRegion','CopySubresourceRegion1','VSSetShader','PSSetShader','Begin','End'}
allowed={'IASetInputLayout','IASetVertexBuffers','IASetIndexBuffer','VSSetConstantBuffers','VSSetConstantBuffers1'}
lines=['// Generated typed SDK methods. Do not edit.'];hooks=[]
for slot,(ret,name,params,names) in enumerate(methods):
    if slot<7 or name.startswith('Get') or re.match(r'^(VS|PS|GS|HS|DS|CS|IA|OM|RS|SO)Get',name):continue
    hooks.append((slot,name))
    lines.append(f'using Fn_{name}={ret}(STDMETHODCALLTYPE*)({params}); static Fn_{name} original_{name}{{}};')
    if name in special:
        lines.append(f'static {ret} STDMETHODCALLTYPE Hook_{name}({params});')
        continue
    body='std::lock_guard<std::recursive_mutex> lock(gate);'
    if name not in allowed:body+=' BeforeBoundary();'
    if name in ('ExecuteCommandList','UpdateTileMappings','CopyTileMappings','CopyTiles','UpdateTiles','ResizeTilePool'):body+=' if(!inside) ++bufferEpoch;'
    if name=='DiscardResource':body+=' Invalidate(pResource);'
    if name in ('DiscardView','DiscardView1'):body+=' InvalidateView(pResourceView);'
    if name in ('ClearState','SwapDeviceContextState','ExecuteCommandList'):body+=' ResetBindings(This);'
    for target in ('pDstResource','pDstBuffer'):
        if target in names:body+=f' Invalidate({target});'
    body+=f' {"return " if ret!="void" else ""}original_{name}({",".join(names)});'
    lines.append(f'static {ret} STDMETHODCALLTYPE Hook_{name}({params}){{{body}}}')
lines.append('static HookSpec contextHooks[]={'+','.join('{%d,reinterpret_cast<void*>(Hook_%s),reinterpret_cast<void**>(&original_%s)}'%(slot,name,name) for slot,name in hooks)+'};')
a.output.write_text('\n'.join(lines)+'\n')
print(f'Generated {len(hooks)} context boundaries from {len(methods)} SDK methods')
