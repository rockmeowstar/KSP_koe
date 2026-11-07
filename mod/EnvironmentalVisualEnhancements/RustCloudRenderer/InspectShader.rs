#[path = "ShaderProgram.rs"]
mod program;
fn main() {
    let mut failed = false;
    for path in std::env::args().skip(1) {
        let result=std::fs::read(&path).map_err(|error| format!("{error}"))
            .and_then(|bytes| {
                let shader=program::Program::parse(&bytes).map_err(|error| format!("{error:?}"))?;
                let reconstructed:Vec<u8>=shader.instructions.iter().flat_map(|instruction|shader.instruction_bytes(instruction).iter().copied()).collect();
                if reconstructed!=bytes[shader.code.start+8..shader.code.end] {return Err("instruction round trip differs".into());}
                let histogram=shader.histogram().iter().map(|(opcode,count)| format!("\"{opcode}\":{count}")).collect::<Vec<_>>().join(",");
                let tags=shader.chunks.iter().map(|chunk|format!("{:?}",chunk.kind)).collect::<Vec<_>>().join(",");
                Ok(format!("{{\"stage\":{},\"major\":{},\"minor\":{},\"bytes\":{},\"chunks\":{},\"chunkTags\":[{}],\"instructions\":{},\"opcodes\":{{{}}}}}",
                    shader.stage,shader.major,shader.minor,bytes.len(),shader.chunks.len(),tags,shader.instructions.len(),histogram))
            });
        match result {
            Ok(summary) => println!("{summary}"),
            Err(error) => {
                eprintln!("{path}: {error}");
                failed = true;
            }
        }
    }
    if failed {
        std::process::exit(1);
    }
}
