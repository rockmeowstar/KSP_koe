#[path = "HalfAnalysis.rs"]
mod analysis;
#[path = "ShaderIr.rs"]
mod ir;
#[path = "HalfOptimizer.rs"]
mod optimizer;
#[path = "ShaderProgram.rs"]
mod program;
fn main() {
    let args: Vec<_> = std::env::args().collect();
    if args.len() < 3 || args.len() > 4 {
        eprintln!("Usage: OptimizeCloudHalf input.dxbc output.dxbc [--branch]");
        std::process::exit(2);
    }
    let run = || -> Result<(), String> {
        let data = std::fs::read(&args[1]).map_err(|error| error.to_string())?;
        let program = program::Program::parse(&data).map_err(|error| format!("{error:?}"))?;
        let _source_model = (program.major, program.minor, program.histogram().len());
        let repair = if args.get(3).is_some_and(|arg| arg == "--branch") {
            optimizer::Repair::Branch
        } else {
            optimizer::Repair::Select
        };
        let result = optimizer::optimize(&program, repair)?;
        std::fs::write(&args[2], &result.bytes).map_err(|error| error.to_string())?;
        let s = result.stats;
        println!("{{\"before\":{},\"after\":{},\"replacements\":{},\"proofs\":{},\"evaluations\":{},\"dead_removed\":{},\"temps_before\":{},\"temps_after\":{},\"bytes\":{}}}",s.before,s.after,s.replacements,s.proofs,s.evaluations,s.removed,s.temps_before,s.temps_after,result.bytes.len());
        Ok(())
    };
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
