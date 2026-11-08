#![allow(dead_code)]
use crate::{program, ir};
use ir::Op;
use program::Program;

fn declaration(op: &Op) -> bool { (88..=106).contains(&op.opcode) || op.opcode==53 }
fn remap(op: &Op, offset: usize, first: bool, result: usize) -> Result<Op,String> {
    let mut w=op.words.clone();
    for operand in &op.operands {
        for &(at,index) in &operand.positions { w[at]=(index+offset) as u32; }
        if first && operand.kind==2 {
            if operand.index!=Some(0) || op.destinations!=1 {
                return Err("first pass has unsupported output".into());
            }
            w[operand.start]=(w[operand.start]&!(255<<12))|(0<<12);
            w[operand.start+1]=result as u32;
        }
        if first && matches!(operand.kind,6|7|8) {
            let token=w[operand.start];
            if (token>>22)&7!=0 {return Err("relative binding index".into());}
            let mut at=operand.start+1;let mut extension=token;
            while extension>>31!=0 {extension=*w.get(at).ok_or("binding extension")?;at+=1;}
            w[at]+=match operand.kind {6=>8,7=>16,8=>5,_=>0};
        }
    }
    Op::decode(w)
}
fn fuse(a:&Program,b:&Program,slot:usize,keep_first:bool)->Result<(Vec<Op>,usize),String> {
    if a.stage!=0 || b.stage!=0 || a.major!=b.major || a.minor!=b.minor {return Err("shader stage mismatch".into());}
    let aa=ir::decode(a)?;let bb=ir::decode(b)?;
    let temp=|ops:&[Op]|->Result<usize,String>{Ok(ops.iter().find(|op|op.opcode==104).ok_or("no temporaries")?.words[1] as usize)};
    let na=temp(&aa)?;let nb=temp(&bb)?;let output=na+nb;
    if output>=4095{return Err("temporary budget".into());}
    let shared=|ops:&[Op]|ops.iter().filter(|op|declaration(op)&&!matches!(op.opcode,88|89|90|104)).map(|op|op.words.clone()).collect::<Vec<_>>();
    if shared(&aa)!=shared(&bb) {return Err("input/output/custom declarations differ".into());}
    let mut out=Vec::new();
    for op in &bb {if declaration(op)&&op.opcode!=104 {out.push(op.clone());}}
    for op in &aa {
        if matches!(op.opcode,88|89|90) {
            let mut words=op.words.clone();
            if words.len()<3 || words[1]>>31!=0 || (words[1]>>22)&7!=0 {return Err("binding declaration".into());}
            let old=words[2];let add=match op.opcode{88=>16,89=>5,90=>8,_=>0};
            let limit=match op.opcode{88=>128,89=>14,90=>16,_=>0};
            if old+add>=limit {return Err("binding budget".into());}
            words[2]+=add;out.push(Op::decode(words)?);
        }
    }
    out.push(Op::decode(vec![(2<<24)|104,(output+1)as u32])?);
    if keep_first {out.push(Op::decode(vec![(3<<24)|101,0x001020f2,1])?);}
    out.push(Op::decode(vec![(1<<24)|48])?);
    let mut loops=0;let mut returns=0;
    for op in aa.iter().filter(|op|!declaration(op)) {
        if op.opcode==48 {loops+=1;}
        if op.opcode==22 {loops-=1;}
        if op.opcode==62 {
            if loops!=0{return Err("return from nested loop".into());}
            out.push(Op::decode(vec![(1<<24)|2])?);returns+=1;
        }else{out.push(remap(op,0,true,output)?);}
    }
    if loops!=0 || returns==0{return Err("invalid first-pass flow".into());}
    out.push(Op::decode(vec![(1<<24)|22])?);
    let mut replaced=0;
    for op in bb.iter().filter(|op|!declaration(op)) {
        let resources:Vec<_>=op.operands.iter().filter(|arg|arg.kind==7&&arg.index==Some(slot)).collect();
        if !resources.is_empty() {
            if op.opcode!=45 || op.operands.len()!=3 || op.operands[0].mask!=15 || resources.len()!=1 {
                return Err("intermediate input is not a full texel load".into());
            }
            let mapped=remap(op,na,false,output)?;let dst=&mapped.operands[0];
            let mut words=vec![54];words.extend_from_slice(&mapped.words[dst.start..dst.end]);words.extend([0x00100e46,output as u32]);
            words[0]|=(words.len() as u32)<<24;out.push(Op::decode(words)?);replaced+=1;
        } else {
            if keep_first && op.opcode==62 {out.push(Op::decode(vec![(5<<24)|54,0x001020f2,1,0x00100e46,output as u32])?);}
            out.push(remap(op,na,false,output)?);
        }
    }
    if replaced!=1{return Err("expected exactly one intermediate load".into());}
    ir::successors(&out)?;Ok((out,replaced))
}
fn emit(p:&Program,ops:&[Op],keep_first:bool)->Result<Vec<u8>,String>{
    let mut words=vec![u32::from_le_bytes(p.original[p.code.start..p.code.start+4].try_into().unwrap()),0];
    for op in ops {words.extend_from_slice(&op.words);}words[1]=words.len() as u32;
    let code:Vec<_>=words.iter().flat_map(|w|w.to_le_bytes()).collect();let mut chunks=Vec::new();
    for c in &p.chunks {if matches!(&c.kind,b"SHDR"|b"SHEX") {let mut x=c.kind.to_vec();x.extend((code.len()as u32).to_le_bytes());x.extend(&code);chunks.push(x);}
        else if &c.kind==b"OSGN" && keep_first {
            let data=&p.original[c.bytes.start+8..c.bytes.end];
            let read=|at:usize|u32::from_le_bytes(data[at..at+4].try_into().unwrap());
            if data.len()<33 || read(0)!=1 || read(4)!=8 || read(12)!=0 || read(24)!=0 {return Err("unsupported output signature".into());}
            let mut payload=Vec::new();payload.extend(2u32.to_le_bytes());payload.extend(8u32.to_le_bytes());
            let mut record=data[8..32].to_vec();record[..4].copy_from_slice(&(read(8)+24).to_le_bytes());payload.extend(&record);
            record[4..8].copy_from_slice(&1u32.to_le_bytes());record[16..20].copy_from_slice(&1u32.to_le_bytes());payload.extend(&record);payload.extend(&data[32..]);
            let mut chunk=b"OSGN".to_vec();chunk.extend((payload.len()as u32).to_le_bytes());chunk.extend(payload);chunks.push(chunk);
        }else if !matches!(&c.kind,b"STAT"|b"SDBG"|b"SPDB"){chunks.push(p.original[c.bytes.clone()].to_vec());}}
    let mut data=vec![0u8;32+4*chunks.len()];data[..4].copy_from_slice(b"DXBC");data[20..24].copy_from_slice(&1u32.to_le_bytes());data[28..32].copy_from_slice(&(chunks.len()as u32).to_le_bytes());
    for(i,c)in chunks.into_iter().enumerate(){let at=data.len()as u32;data[32+i*4..36+i*4].copy_from_slice(&at.to_le_bytes());data.extend(c);while data.len()%4!=0{data.push(0);}}
    let size=data.len()as u32;data[24..28].copy_from_slice(&size.to_le_bytes());Program::parse(&data).map_err(|e|format!("{e:?}"))?;Ok(data)
}
fn main()->Result<(),Box<dyn std::error::Error>>{
    let args:Vec<_>=std::env::args().collect();if args.len()!=5&&args.len()!=6{return Err("usage: CloudFuse first.dxbc second.dxbc input-slot output.dxbc [--keep-first]".into());}
    if std::path::Path::new(&args[4]).exists(){return Err("output exists".into());}
    let a=std::fs::read(&args[1])?;let b=std::fs::read(&args[2])?;
    let pa=Program::parse(&a).map_err(|e|format!("{e:?}"))?;let pb=Program::parse(&b).map_err(|e|format!("{e:?}"))?;
    let keep=args.get(5).is_some_and(|x|x=="--keep-first");
    let (ops,n)=fuse(&pa,&pb,args[3].parse()?,keep)?;std::fs::write(&args[4],emit(&pb,&ops,keep)?)?;
    println!("{{\"replaced_loads\":{n},\"first_cb_offset\":5,\"first_srv_offset\":16,\"first_sampler_offset\":8,\"instructions\":{}}}",ops.len());Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn op(words: &[u32])->Op {Op::decode(words.to_vec()).unwrap()}
    #[test]
    fn source_binding_relocation_preserves_negation_and_row() {
        let source=op(&[(7<<24)|54,0x001000f2,0,0x80208e46,0x41,0,37]);
        let moved=remap(&source,0,true,8).unwrap();
        assert_eq!(moved.words[5],5);assert_eq!(moved.words[6],37);
        assert_eq!(moved.operands[1].modifier,source.operands[1].modifier);
    }
    #[test]
    fn packed_output_components_keep_their_masks() {
        for mask in 1..16 {
            let source=op(&[(5<<24)|54,0x00102002|(mask<<4),0,0x00100e46,2]);
            let moved=remap(&source,0,true,8).unwrap();
            assert_eq!(moved.operands[0].kind,0);assert_eq!(moved.operands[0].index,Some(8));
            assert_eq!(moved.operands[0].mask,mask as u8);
        }
    }
    #[test]
    fn second_pass_registers_have_disjoint_indices() {
        let source=op(&[(5<<24)|54,0x001000f2,2,0x00100e46,7]);
        let moved=remap(&source,40,false,99).unwrap();
        assert_eq!(moved.operands[0].index,Some(42));assert_eq!(moved.operands[1].index,Some(47));
    }
    #[test]
    fn additional_render_targets_are_rejected() {
        let source=op(&[(5<<24)|54,0x001020f2,1,0x00100e46,2]);
        assert!(remap(&source,0,true,8).is_err());
    }
    #[test]
    fn ffi_rejects_null_or_truncated_inputs_without_publishing_output() {
        unsafe {
            let mut output=std::ptr::dangling::<u8>();let mut length=99;
            assert!(koe_fuse_cloud_passes(std::ptr::null(),32,std::ptr::null(),32,&mut output,&mut length).is_null());
            assert!(output.is_null());assert_eq!(length,0);
            let data=[0u8;32];output=data.as_ptr();length=99;
            assert!(koe_fuse_cloud_passes(data.as_ptr(),data.len(),data.as_ptr(),31,&mut output,&mut length).is_null());
            assert!(output.is_null());assert_eq!(length,0);
            assert!(koe_fuse_cloud_passes(data.as_ptr(),32,data.as_ptr(),32,&mut output,&mut length).is_null());
            assert!(output.is_null());assert_eq!(length,0);
            koe_free_fused_cloud(std::ptr::null_mut());
        }
    }
}
pub struct FusedCode { bytes: Vec<u8> }
#[no_mangle]
pub unsafe extern "C" fn koe_fuse_cloud_passes(
    first:*const u8, first_len:usize, second:*const u8, second_len:usize,
    output:*mut *const u8, output_len:*mut usize
)->*mut FusedCode{
    if output.is_null()||output_len.is_null(){return std::ptr::null_mut();}
    *output=std::ptr::null();*output_len=0;
    if first.is_null()||second.is_null()||first_len<32||second_len<32||first_len>u32::MAX as usize||second_len>u32::MAX as usize{return std::ptr::null_mut();}
    let result=std::panic::catch_unwind(||{
        let a=Program::parse(std::slice::from_raw_parts(first,first_len)).ok()?;
        let b=Program::parse(std::slice::from_raw_parts(second,second_len)).ok()?;
        let (ops,_)=fuse(&a,&b,7,true).ok()?;
        Some(emit(&b,&ops,true).ok()?)
    });
    let bytes=match result{Ok(Some(bytes))=>bytes,_=>return std::ptr::null_mut()};
    let code=Box::new(FusedCode{bytes});
    *output=code.bytes.as_ptr();*output_len=code.bytes.len();Box::into_raw(code)
}
#[no_mangle]
pub unsafe extern "C" fn koe_free_fused_cloud(code:*mut FusedCode){if !code.is_null(){drop(Box::from_raw(code));}}
