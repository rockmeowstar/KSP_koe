using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using HarmonyLib;
using CloudLoopFix;

static class EveTextureTests
{
    static string game;static int checks;
    static void Check(bool value,string name){checks++;if(!value)throw new Exception(name);}
    static Assembly Resolve(object sender,ResolveEventArgs args)
    {
        string name=new AssemblyName(args.Name).Name+".dll";
        foreach(var folder in new[]{@"KSP_x64_Data\Managed",@"GameData\000_Harmony",@"GameData\EnvironmentalVisualEnhancements\Plugins"})
        {string path=Path.Combine(game,folder,name);if(File.Exists(path))return Assembly.Load(File.ReadAllBytes(path));}
        return null;
    }
    static int Main(string[] args)
    {try{game=args[0];AppDomain.CurrentDomain.AssemblyResolve+=Resolve;Run();return 0;}catch(Exception ex){Console.Error.WriteLine(ex);return 1;}}
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        var random=new Random(4713);
        foreach(int size in new[]{0,128,148,4096,1048576})foreach(int offset in new[]{0,128,148})
        {
            if(offset>size)continue;var bytes=new byte[size];random.NextBytes(bytes);
            foreach(int count in new[]{0,1,size,size+7})
            using(var stream=EveTexturePatch.Open(bytes))using(var reader=new BinaryReader(stream))
            using(var baseline=new BinaryReader(new MemoryStream(bytes)))
            {
                stream.Position=offset;baseline.BaseStream.Position=offset;var expected=baseline.ReadBytes(count);
                using(var payload=new EvePayload(reader,count))
                {
                    byte[] actual=new byte[payload.Count];Marshal.Copy(payload.Pointer,actual,0,actual.Length);
                    Check(expected.SequenceEqual(actual),"DDS payload exact bytes");Check(stream.Position==baseline.BaseStream.Position,"Short read position");
                    if(payload.Count>0){bytes[offset]^=1;Check(Marshal.ReadByte(payload.Pointer)==bytes[offset],"Original array pinned");bytes[offset]^=1;}
                }
            }
        }
        bool rejected=false;try{using(var reader=new BinaryReader(EveTexturePatch.Open(new byte[1])))using(var p=new EvePayload(reader,-1)){} }catch(ArgumentOutOfRangeException){rejected=true;}
        Check(rejected,"Negative read rejected");
        var utils=Assembly.Load(File.ReadAllBytes(Path.Combine(game,@"GameData\EnvironmentalVisualEnhancements\Plugins\Utils.dll")));
        Check(utils.ManifestModule.ModuleVersionId==new Guid("752c32c2-9c32-40be-bad9-8a27f80a0a26"),"Known EVE Utils fixture");
        var target=AccessTools.DeclaredMethod(utils.GetType("Utils.TextureConverter",true),"DDSToTexture");
        var original=PatchProcessor.GetOriginalInstructions(target);var patched=EveTexturePatch.Transpile(original).ToList();
        Check(patched.Count==original.Count,"IL coordinates retained");
        Check(patched.Count(c=>c.Calls(AccessTools.Method(typeof(EveTexturePatch),"Upload")))==14,"All 14 original upload branches");
        Check(!patched.Any(c=>c.Calls(typeof(BinaryReader).GetMethod("ReadBytes",new[]{typeof(int)}))),"No DDS payload clone remains");
        var changed=original.Select(c=>new CodeInstruction(c)).ToList();var upload=changed.First(c=>c.Calls(typeof(BinaryReader).GetMethod("ReadBytes",new[]{typeof(int)})));upload.opcode=System.Reflection.Emit.OpCodes.Nop;upload.operand=null;
        rejected=false;try{EveTexturePatch.Transpile(changed).ToList();}catch(NotSupportedException){rejected=true;}Check(rejected,"Missing branch rejects patch");
        string folder=Path.Combine(Path.GetTempPath(),"KOE-EVE-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(folder);
        try
        {
            string a=Path.Combine(folder,"a.dds"),b=Path.Combine(folder,"b.dds"),c=Path.Combine(folder,"c.dds");
            File.WriteAllBytes(a,new byte[]{1,2,3});File.WriteAllBytes(b,new byte[]{4,5,6,7});File.WriteAllBytes(c,new byte[50]);
            using(var bank=new EarlyFileBank(new[]{a,b,c,a,Path.Combine(folder,"absent")},7,4))
            {
                Check(bank.WaitForWorkers(5000),"Workers complete");Check(bank.ReservedBytes==7&&bank.SelectedFiles==2,"Budget, duplicate and missing-file guards");
                byte[] data;Check(bank.TryTake(b,out data)&&data.SequenceEqual(new byte[]{4,5,6,7}),"Unordered take");
                Check(!bank.TryTake(b,out data),"Single consumption");
                File.WriteAllBytes(a,new byte[]{9});Check(!bank.TryTake(a,out data),"Changed file invalidates prepared bytes");
                bank.Dispose();Check(!bank.TryTake(c,out data),"Disposed bank misses safely");
            }
        }
        finally
        {
            if(!Path.GetFullPath(folder).StartsWith(Path.GetFullPath(Path.GetTempPath())+"KOE-EVE-",StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Unsafe test cleanup path");
            Directory.Delete(folder,true);
        }
        Console.WriteLine("EVE_TEXTURE_PIPELINE_PASS checks="+checks+"; native uploads not executed");
    }
}
