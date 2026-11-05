// AI-generated comment: Used only when compiling source-level algorithm/IL tests, never packaged.
namespace KerbalOverEngineering.Options
{
    public static class Settings
    {
        public static readonly System.Collections.Generic.HashSet<string> Disabled = new System.Collections.Generic.HashSet<string>();
        public static bool Enabled(string key)
        {
            return !Disabled.Contains(key);
        }
    }
}
