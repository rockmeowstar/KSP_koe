using System;
using System.IO;
using System.Linq;
using KoeFrameGeneration;

static class FrameGenerationSettingsTests
{
    static void Require(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }
    static void Main()
    {
        string root = Path.Combine(Path.GetTempPath(), "KOE-frame-settings-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(root, "FrameGeneration.cfg");
        try
        {
            var defaults = FrameGenerationSettings.Load(path, 74.983);
            Require(!defaults.FullGeneration && Math.Abs(defaults.TargetFPS-74.983)<.0001 && File.Exists(path), "Missing config did not create conservative mode with the monitor target");
            Require(File.ReadAllText(path).Contains("FullGeneration = false"), "Conservative mode is not visible in the config");
            foreach (int value in new[] { 30, 59, 60, 75, 144, int.MaxValue })
            {
                File.WriteAllText(path, "# Manual choice\nTargetFPS = " + value + "\n");
                byte[] before = File.ReadAllBytes(path);
                var settings=FrameGenerationSettings.Load(path,144);
                Require(settings.TargetFPS==value && !settings.FullGeneration, "An old target-only file enabled full generation");
                Require(before.SequenceEqual(File.ReadAllBytes(path)), "Reading settings changed the manual file");
            }
            foreach (string text in new[] { "TargetFPS = broken", "TargetFPS = 60\nTargetFPS = 75", "# TargetFPS = 120", "Unknown = 90", "TargetFPS = NaN", "TargetFPS = Infinity", "TargetFPS = -1", "TargetFPS = 0" })
            {
                File.WriteAllText(path, text);
                Require(FrameGenerationSettings.Load(path,144).TargetFPS == 144, "Invalid or missing target did not use monitor refresh");
            }
            foreach(string text in new[]{"FullGeneration = invalid", "FullGeneration = true\nFullGeneration = true"})
            {
                File.WriteAllText(path,text);
                Require(!FrameGenerationSettings.Load(path,144).FullGeneration,"Invalid or duplicated mode enabled experimental generation");
            }
            File.WriteAllText(path,"FullGeneration = true\nTargetFPS = 59.94\n");
            var experimental=FrameGenerationSettings.Load(path,144);
            Require(experimental.FullGeneration && experimental.TargetFPS==59.94,"Explicit experimental settings were ignored");
            File.Delete(path);
            Require(!FrameGenerationSettings.Load(path,144).FullGeneration && FrameGenerationSettings.Load(path,144).TargetFPS==144, "Deleted config was not recreated in conservative mode");
            File.Delete(path);
            Require(FrameGenerationSettings.Load(path,0).TargetFPS==60,"An unknown monitor did not use a safe default");
            Console.WriteLine("Frame settings: conservative defaults, monitor target, experimental opt-in, invalid values and recreation passed.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }
}
