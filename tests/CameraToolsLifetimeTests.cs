using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using HarmonyLib;
using KSP.UI.Screens;
using KoeCameraToolsLifetime;

static class CameraToolsLifetimeTests
{
    static string game;
    static int checks;
    static void Check(bool value, string message) { checks++; if (!value) throw new Exception(message); }
    static void OtherCallback() { }
    static Assembly Resolve(object sender, ResolveEventArgs args)
    {
        string name = new AssemblyName(args.Name).Name;
        var loaded = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == name);
        if (loaded != null) return loaded;
        foreach (string directory in new[] { "KSP_x64_Data/Managed", "GameData/000_Harmony", "GameData/CameraTools/Plugins", "GameData/KerbalOverEngineering/Plugins" })
        { string path = Path.Combine(game, directory, name+".dll"); if (File.Exists(path)) return Assembly.Load(File.ReadAllBytes(path)); }
        return null;
    }
    static int Main(string[] args)
    {
        game=args[0]; AppDomain.CurrentDomain.AssemblyResolve+=Resolve;
        try { Run(); Console.WriteLine("CameraTools lifetime: "+checks+" checks passed; managed callbacks only"); return 0; }
        catch(Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    static void Run()
    {
        var assembly=Assembly.Load(File.ReadAllBytes(Path.Combine(game,"GameData/CameraTools/Plugins/CameraTools.dll")));
        var type=assembly.GetType("CameraTools.CamTools",true); Bootstrap.Prepare(type);
        var method=AccessTools.DeclaredMethod(type,"AddToolbarButton");
        var source=PatchProcessor.GetOriginalInstructions(method).ToList();
        var rewritten=Bootstrap.CaptureButton(source).ToList();
        Check(rewritten.Count==source.Count+1,"Capture consumes the discarded button without changing the call arguments");
        Check(rewritten.Count(c=>c.Calls(AccessTools.Method(typeof(Bootstrap),"Track")))==1,"One returned button is captured");
        var button=(ApplicationLauncherButton)FormatterServices.GetUninitializedObject(typeof(ApplicationLauncherButton));
        var onTrue=AccessTools.Field(typeof(ApplicationLauncherButton),"onTrue");
        onTrue.SetValue(button,new Callback(OtherCallback));
        object previous=null;
        for(int cycle=0;cycle<100;cycle++)
        {
            var owner=FormatterServices.GetUninitializedObject(type);
            Bootstrap.Track(button,owner);
            var calls=((Delegate)onTrue.GetValue(button)).GetInvocationList();
            Check(calls.Length==2 && calls.Count(c=>ReferenceEquals(c.Target,owner))==1,"One live owner and the unrelated callback remain");
            if(previous!=null)
            {
                Bootstrap.Detach(previous,null);
                Check(((Delegate)onTrue.GetValue(button)).GetInvocationList().Any(c=>ReferenceEquals(c.Target,owner)),"Old destruction does not detach the new owner");
            }
            AccessTools.Field(type,"fetch").SetValue(null,owner);
            Bootstrap.Detach(owner,null);
            Check(((Delegate)onTrue.GetValue(button)).GetInvocationList().Length==1,"Destroyed controller is no longer retained by the button");
            Check(AccessTools.Field(type,"fetch").GetValue(null)==null,"Destroyed singleton is released");
            var error=new Exception("original");
            Check(ReferenceEquals(Bootstrap.Detach(owner,error),error),"Original destruction error is preserved");
            previous=owner;
        }
    }
}
