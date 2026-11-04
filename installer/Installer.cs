using System;
using System.IO;
using System.Reflection;
using System.Collections.Generic;
using System.Diagnostics;

// Optional convenience. The supported primary installation is copying GameData.
static class Installer
{
    static byte[] Resource(string name)
    {
        using (var input = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
        using (var output = new MemoryStream()) { if (input == null) throw new FileNotFoundException(name); input.CopyTo(output); return output.ToArray(); }
    }
    static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("Usage: KerbalOverEngineering.exe <KSP directory>. Or copy GameData into KSP."); return 2; }
        var changed=new List<string>();
        var originals=new Dictionary<string,byte[]>();
        try
        {
            string game = Path.GetFullPath(args[0]);
            if (!File.Exists(Path.Combine(game, "KSP_x64.exe"))) throw new DirectoryNotFoundException("KSP_x64.exe not found");
            foreach(var process in Process.GetProcessesByName("KSP_x64"))
            {
                try {if(string.Equals(Path.GetDirectoryName(process.MainModule.FileName),game,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Close this KSP instance before installation");}
                finally {process.Dispose();}
            }
            string root = Path.GetFullPath(Path.Combine(game, "GameData/KerbalOverEngineering")) + Path.DirectorySeparatorChar;
            string backup = Path.Combine(game, "KerbalOverEngineering-backups", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fffffff"));
            string[] manifest = System.Text.Encoding.UTF8.GetString(Resource("package.manifest")).Split(new[] {'\r','\n'}, StringSplitOptions.RemoveEmptyEntries);
            var payloads=new Dictionary<string,byte[]>();
            // Validate every destination/resource before changing the first file.
            foreach (string relative in manifest)
            {
                if (relative == "Options.cfg") throw new InvalidDataException("User config must not be packaged");
                string path = Path.GetFullPath(Path.Combine(root, relative));
                if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Unsafe package path");
                byte[] bytes = Resource("package." + relative.Replace('/', '.'));
                payloads.Add(path,bytes);
            }
            // Retire only explicitly owned previous plugin locations. The real
            // Waterfall mod and all unknown files remain untouched. Every retired
            // file is backed up and participates in the same rollback transaction.
            foreach(string relative in new[]{
                "GameData/KerbalOverEngineering/mod/Waterfall/LightBatchFix/Plugins/WaterfallLightBatchFix.dll",
                "GameData/KerbalOverEngineering/mod/Waterfall/LightBatchFix/Settings.cfg",
                "GameData/WaterfallLightBatchFix/Plugins/WaterfallLightBatchFix.dll",
                "GameData/WaterfallLightBatchFix/Settings.cfg",
                "GameData/ExhaustSyncFix/Plugins/ExhaustSyncFix.dll",
                "GameData/KerbalOverEngineeringExperiments/SceneLoadBenchmark.dll",
                "GameData/KerbalOverEngineeringExperiments/InstancedLights/KoeInstancedLightsExperiment.dll",
                "GameData/KerbalOverEngineeringExperiments/FrameGeneration/KoeFrameGenerationBridge.dll",
                "GameData/KerbalOverEngineeringExperiments/CloudGuards/KoeCloudGuardExperiment.dll",
                "KSP_x64_Data/Plugins/KoeCloudGuard.dll",
                "KSP_x64_Data/Plugins/KoeInstancedLights.dll",
                "KSP_x64_Data/Plugins/KoeFrameGeneration.dll",
                "KSP_x64_Data/Plugins/KoeGpuTimeProbe.dll"
            })
            {
                string path=Path.GetFullPath(Path.Combine(game,relative));
                if(!File.Exists(path))continue;
                byte[] bytes=File.ReadAllBytes(path);originals.Add(path,bytes);
                string archived=Path.Combine(backup,"retired",relative);
                Directory.CreateDirectory(Path.GetDirectoryName(archived));File.WriteAllBytes(archived,bytes);
                File.Delete(path);changed.Add(path);
            }
            foreach(var entry in payloads)
            {
                string path=entry.Key;byte[] bytes=entry.Value;
                string relative=path.Substring(root.Length);
                originals.Add(path,File.Exists(path)?File.ReadAllBytes(path):null);
                if (File.Exists(path))
                {
                    string copy = Path.Combine(backup, relative); Directory.CreateDirectory(Path.GetDirectoryName(copy)); File.Copy(path, copy);
                }
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                string temporary = path + ".new-"+Guid.NewGuid().ToString("N");
                try
                {
                    File.WriteAllBytes(temporary, bytes);
                    if (File.Exists(path)) File.Replace(temporary, path, null); else File.Move(temporary, path);
                    changed.Add(path);
                }
                finally {if(File.Exists(temporary))File.Delete(temporary);}
            }
            Console.WriteLine("Installed. Options.cfg will be generated only if missing on the next KSP launch."); return 0;
        }
        catch (Exception error)
        {
            changed.Reverse();
            foreach(string path in changed)
                try {if(originals[path]==null)File.Delete(path);else File.WriteAllBytes(path,originals[path]);}
                catch(Exception rollback){Console.Error.WriteLine("Rollback failed: "+path+": "+rollback.Message);}
            Console.Error.WriteLine(error.Message); return 1;
        }
    }
}
