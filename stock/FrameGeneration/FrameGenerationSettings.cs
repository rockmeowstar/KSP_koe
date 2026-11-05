using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace KoeFrameGeneration
{
    public sealed class FrameGenerationSettings
    {
        public const int ConservativeTargetFPS = 60;
        public bool FullGeneration { get; private set; }
        public double TargetFPS { get; private set; }

        public static FrameGenerationSettings Load(string path, double monitorHz)
        {
            double defaultTarget = !double.IsNaN(monitorHz) && !double.IsInfinity(monitorHz) && monitorHz > 0
                ? monitorHz : ConservativeTargetFPS;
            if (!File.Exists(path))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
                string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    File.WriteAllText(temporary,
                        "// Default: target 60 FPS, or monitor refresh if lower.\nFullGeneration = false\n"
                        + "// Experimental: enable FullGeneration to fill toward TargetFPS.\n"
                        + "// Both modes are limited to monitor refresh and 3 extra frames per real frame.\nTargetFPS = "
                        + defaultTarget.ToString("0.###", CultureInfo.InvariantCulture) + "\n",
                        new UTF8Encoding(false));
                    try { File.Move(temporary, path); }
                    catch (IOException) { if (!File.Exists(path)) throw; }
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }
            var result = new FrameGenerationSettings { TargetFPS = defaultTarget };
            int modeEntries = 0, targetEntries = 0;
            foreach (string source in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = source.Trim();
                if (line.StartsWith("//") || line.StartsWith("#")) continue;
                int separator = line.IndexOf('=');
                if (separator < 0) continue;
                string key = line.Substring(0, separator).Trim();
                string value = line.Substring(separator + 1).Trim();
                if (string.Equals(key, "FullGeneration", StringComparison.OrdinalIgnoreCase))
                {
                    bool enabled;
                    modeEntries++;
                    result.FullGeneration = modeEntries == 1 && bool.TryParse(value, out enabled) && enabled;
                }
                else if (string.Equals(key, "TargetFPS", StringComparison.OrdinalIgnoreCase))
                {
                    double target;
                    targetEntries++;
                    result.TargetFPS = targetEntries == 1
                        && double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out target)
                        && !double.IsNaN(target) && !double.IsInfinity(target) && target > 0 ? target : defaultTarget;
                }
            }
            return result;
        }
    }
}
