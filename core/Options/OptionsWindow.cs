using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using KSP.UI.Screens;
using UnityEngine;

namespace KerbalOverEngineering.Options
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class OptionsWindow : MonoBehaviour
    {
        ApplicationLauncherButton button;
        Texture2D icon;
        Rect window = new Rect(200, 100, 700, 600);
        Vector2 scroll;
        readonly Dictionary<string, bool> values = new Dictionary<string, bool>(StringComparer.Ordinal);
        readonly Dictionary<string, bool> loadedValues = new Dictionary<string, bool>(StringComparer.Ordinal);
        Dictionary<string, string> versionNotices = new Dictionary<string, string>(StringComparer.Ordinal);
        byte[] loadedFile;
        string selected = "Stock", error = "";
        bool visible, dirty;

        public void Awake()
        {
            DontDestroyOnLoad(gameObject);
            Settings.Enabled("Stock.ExhaustSync");
            Load();
            GameEvents.onGUIApplicationLauncherReady.Add(AddButton);
            GameEvents.onGUIApplicationLauncherDestroyed.Add(RemoveButton);
            if (ApplicationLauncher.Ready) AddButton();
        }

        void AddButton()
        {
            if (button != null || ApplicationLauncher.Instance == null) return;
            icon = MakeIcon();
            button = ApplicationLauncher.Instance.AddModApplication(
                Open, () => visible = false, null, null, null, null,
                ApplicationLauncher.AppScenes.SPACECENTER | ApplicationLauncher.AppScenes.FLIGHT |
                ApplicationLauncher.AppScenes.VAB | ApplicationLauncher.AppScenes.SPH,
                icon);
        }

        void Open()
        {
            Load();
            versionNotices = ModVersionNotices.Collect();
            dirty = false;
            error = "";
            visible = true;
        }

        void RemoveButton()
        {
            if (button != null && ApplicationLauncher.Instance != null)
                ApplicationLauncher.Instance.RemoveModApplication(button);
            button = null;
            if (icon != null) Destroy(icon);
            icon = null;
        }

        static Texture2D MakeIcon()
        {
            var texture = new Texture2D(38, 38, TextureFormat.RGBA32, false);
            var pixels = Enumerable.Repeat(new Color32(63, 52, 132, 255), 38 * 38).ToArray();
            string[] glyphs = { "101111111", "110101100", "100101110", "110101100", "101111111" };
            for (int y = 0; y < glyphs.Length; y++)
                for (int x = 0; x < glyphs[y].Length; x++)
                    if (glyphs[y][x] == '1')
                        for (int dy = 0; dy < 3; dy++)
                            for (int dx = 0; dx < 3; dx++)
                                pixels[(27 - y * 3 - dy) * 38 + 5 + x * 3 + dx] = new Color32(255, 255, 255, 255);
            texture.SetPixels32(pixels);
            texture.Apply();
            return texture;
        }

        void Load()
        {
            values.Clear();
            foreach (var feature in Catalog.Features) values[feature.Key] = false;
            if (!File.Exists(Settings.ConfigPath)) return;
            loadedFile = File.ReadAllBytes(Settings.ConfigPath);
            string group = "";
            foreach (string source in File.ReadAllLines(Settings.ConfigPath))
            {
                string line = source.Trim();
                if (line.StartsWith("[") && line.EndsWith("]")) { group = line.Substring(1, line.Length - 2).Trim(); continue; }
                if (line.StartsWith("//") || line.StartsWith("#")) continue;
                int split = line.IndexOf('=');
                bool value;
                if (split > 0 && bool.TryParse(line.Substring(split + 1).Trim(), out value))
                {
                    string key = group + "." + line.Substring(0, split).Trim();
                    if (values.ContainsKey(key)) values[key] = value;
                }
            }
            loadedValues.Clear();
            foreach (var pair in values) loadedValues.Add(pair.Key, pair.Value);
        }

        static bool Present(string group)
        {
            if (group == "Stock" || group == "Rendering") return true;
            var names = AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetName().Name).ToArray();
            switch (group)
            {
                case "EVE": return names.Contains("Atmosphere") || names.Contains("Utils");
                case "FAR": return names.Contains("FerramAerospaceResearch");
                case "FreeIva": return names.Contains("FreeIva");
                case "Scatterer": return names.Contains("scatterer");
                default: return names.Any(n => string.Equals(n, group, StringComparison.OrdinalIgnoreCase));
            }
        }

        bool Save()
        {
            string path = Settings.ConfigPath;
            if (loadedFile == null || !loadedFile.SequenceEqual(File.ReadAllBytes(path)))
                throw new InvalidOperationException("Options.cfg changed outside this window; reopen it and try again.");
            string[] source = File.ReadAllLines(path);
            var seen = new HashSet<string>(StringComparer.Ordinal);
            var output = new List<string>();
            string group = "";
            foreach (string original in source)
            {
                string line = original.Trim();
                if (line.StartsWith("[") && line.EndsWith("]")) group = line.Substring(1, line.Length - 2).Trim();
                int split = line.IndexOf('=');
                if (!line.StartsWith("//") && !line.StartsWith("#") && split > 0)
                {
                    string key = group + "." + line.Substring(0, split).Trim();
                    if (values.ContainsKey(key))
                    {
                        if (!seen.Add(key)) throw new InvalidOperationException("Duplicate option: " + key);
                        output.Add(line.Substring(0, split).Trim() + " = " + (values[key] ? "true" : "false"));
                        continue;
                    }
                }
                output.Add(original);
            }
            foreach (var featureGroup in Catalog.Features.Where(f => !seen.Contains(f.Key)).GroupBy(f => f.Group))
            {
                output.Add(""); output.Add("[" + featureGroup.Key + "]");
                foreach (var feature in featureGroup) output.Add(feature.Name + " = " + (values[feature.Key] ? "true" : "false"));
            }
            string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllLines(temp, output, new UTF8Encoding(false));
                File.Replace(temp, path, null);
            }
            finally { if (File.Exists(temp)) File.Delete(temp); }
            loadedFile = File.ReadAllBytes(path);
            bool restart = false;
            foreach (var pair in values)
            {
                bool previous;
                if (!loadedValues.TryGetValue(pair.Key, out previous) || previous == pair.Value) continue;
                if (!LiveOptions.Apply(pair.Key, pair.Value)) restart = true;
            }
            loadedValues.Clear();
            foreach (var pair in values) loadedValues.Add(pair.Key, pair.Value);
            dirty = false;
            return restart;
        }

        void OnGUI()
        {
            if (visible) window = GUILayout.Window(GetInstanceID(), window, Draw, "KOE settings");
        }

        void Draw(int id)
        {
            GUILayout.Label("Live options apply after Save; other changes require restarting KSP.");
            var groups = Catalog.Features.Select(f => f.Group).Distinct().Where(Present).ToArray();
            GUILayout.BeginHorizontal();
            GUILayout.BeginVertical(GUILayout.Width(160));
            foreach (string group in groups)
                if (GUILayout.Toggle(selected == group, group, GUI.skin.button)) selected = group;
            GUILayout.EndVertical();
            scroll = GUILayout.BeginScrollView(scroll);
            string versionNotice;
            if (versionNotices.TryGetValue(selected, out versionNotice))
                GUILayout.Label(versionNotice, new GUIStyle(GUI.skin.label) { wordWrap = true });
            foreach (Feature feature in Catalog.Features.Where(f => f.Group == selected))
            {
                bool previous = values[feature.Key];
                bool active;
                bool live = LiveOptions.TryGet(feature.Key, out active);
                string status = live ? "  ·  Live (active: " + (active ? "on" : "off") + ")" : "  ·  Restart required";
                bool current = GUILayout.Toggle(previous, feature.Name + status);
                if (current != previous) { values[feature.Key] = current; dirty = true; }
                if (feature.Parent != null) GUILayout.Label("Requires " + feature.Parent);
                if (feature.Key == "Stock.RealtimePhysicsCatchup")
                    GUILayout.Label("Adapts the catch-up budget to keep physics close to real time while preserving the stock physics step. Real-time speed depends on available CPU time.",
                        new GUIStyle(GUI.skin.label) { wordWrap = true });
                if (feature.Key == "Stock.PhysicsWarpPrecision")
                    GUILayout.Label("Experimental. Uses the 1x physics step and clock, with tick-based SAS, thermal integration, autopilot and FAR voxel publication. Requires restart. Higher CPU cost; geometry changes can pause physics.",
                        new GUIStyle(GUI.skin.label) { wordWrap = true });
            }
            GUILayout.EndScrollView();
            GUILayout.EndHorizontal();
            if (!string.IsNullOrEmpty(error)) GUILayout.Label(error);
            GUILayout.BeginHorizontal();
            GUI.enabled = dirty;
            if (GUILayout.Button("Save"))
            {
                try { error = Save() ? "Saved. Restart KSP for remaining changes." : "Saved and applied."; }
                catch (Exception ex) { error = "Could not save: " + ex.Message; }
            }
            GUI.enabled = true;
            if (GUILayout.Button("Close")) { visible = false; if (button != null) button.SetFalse(false); }
            GUILayout.EndHorizontal();
            GUI.DragWindow();
        }

        public void OnDestroy()
        {
            GameEvents.onGUIApplicationLauncherReady.Remove(AddButton);
            GameEvents.onGUIApplicationLauncherDestroyed.Remove(RemoveButton);
            RemoveButton();
        }
    }
}
