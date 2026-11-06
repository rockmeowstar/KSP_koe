"""Extract one relocation-free COFF function and its x64 unwind metadata."""
from pathlib import Path
import hashlib
import json
import struct
import sys

source=Path(sys.argv[1]);out=Path(sys.argv[2]);data=source.read_bytes()
machine,sections,_,symbols,count,optional,_=struct.unpack_from('<HHIIIHH',data)
assert machine==0x8664 and optional==0
string_table=symbols+count*18
def long_name(raw):
    if raw[:1]==b'/':
        pos=string_table+int(raw[1:].rstrip(b'\0'));return data[pos:data.index(0,pos)].decode()
    return raw.rstrip(b'\0').decode()
items=[]
for i in range(sections):
    pos=20+i*40
    name=long_name(data[pos:pos+8]);size,raw,relocations=struct.unpack_from('<III',data,pos+16)
    nreloc=struct.unpack_from('<H',data,pos+32)[0]
    items.append(dict(name=name,size=size,raw=raw,relocations=relocations,nreloc=nreloc))
index=0;function=None
while index<count:
    pos=symbols+index*18;name=data[pos:pos+8]
    if name[:4]==b'\0'*4:
        p=string_table+struct.unpack_from('<I',name,4)[0];name=data[p:data.index(0,p)]
    else:name=name.rstrip(b'\0')
    value,section,kind,storage,aux=struct.unpack_from('<IhHBB',data,pos+8)
    if name==b'rust_uniform_update':function=(value,section)
    index+=1+aux
assert function is not None
offset,number=function;section=items[number-1]
assert offset==0 and section['name']=='.text$koe_rust' and section['nreloc']==0,section
code=data[section['raw']:section['raw']+section['size']]
unwinds=[s for s in items if s['name']=='.xdata']
assert len(unwinds)<=1,unwinds
unwind=data[unwinds[0]['raw']:unwinds[0]['raw']+unwinds[0]['size']] if unwinds else b''
assert not unwind or (unwind[0]&7)==1 and (unwind[0]>>3)==0,'Unsupported unwind handler'
out.with_suffix('.bin').write_bytes(code)
source_hash=hashlib.sha256(Path(sys.argv[3]).read_bytes()).hexdigest()
header='#pragma once\nstatic const char RustKernelSourceHash[]="'+source_hash+'";\nstatic const unsigned char RustKernelBytes[]={'+','.join(f'0x{b:02x}' for b in code)+'};\n'
header+='static const unsigned char RustKernelUnwind[]={'+(','.join(f'0x{b:02x}' for b in unwind) if unwind else '0')+'};\n'
header+=f'static const unsigned RustKernelUnwindSize={len(unwind)};\n'
out.write_text(header)
metadata=dict(code_bytes=len(code),unwind_bytes=len(unwind),code_sha256=hashlib.sha256(code).hexdigest(),relocations=0,sections=[dict(name=s['name'],size=s['size'],relocations=s['nreloc']) for s in items])
out.with_suffix('.json').write_text(json.dumps(metadata,indent=2))
print(json.dumps({k:v for k,v in metadata.items() if k!='sections'}))
