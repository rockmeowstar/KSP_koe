using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using HarmonyLib;
using KSP.UI.Screens;
using UnityEngine;

namespace KoeCameraToolsLifetime
{
    [KSPAddon(KSPAddon.Startup.Instantly, true)]
    public sealed class Bootstrap : MonoBehaviour
    {
        const string Id = "KerbalOverEngineering.CameraTools.ToolbarLifetime";
        static Type ownerType;
        static FieldInfo added, fetch;
        static FieldInfo[] callbacks;
        static MethodInfo toggle, dummy;
        static ApplicationLauncherButton button;
        public static long Rebindings, Detachments;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("CameraTools.ToolbarLifetime")) { Destroy(this); return; }
            var harmony = new Harmony(Id);
            try
            {
                var type = AccessTools.TypeByName("CameraTools.CamTools");
                if (type == null) throw new NotSupportedException("CameraTools is not installed");
                string hash;
                using (var sha = SHA256.Create()) using (var stream = File.OpenRead(type.Assembly.Location))
                    hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
                if (hash != "a53344daa2698c2b7365cada4e8065972f008ce0c2f087c688e5d45c09e4b417")
                    throw new NotSupportedException("CameraTools toolbar ownership has not been reviewed for this build");
                Prepare(type);
                harmony.Patch(AccessTools.DeclaredMethod(type, "AddToolbarButton"), new HarmonyMethod(typeof(Bootstrap), "BeforeAdd"), null, new HarmonyMethod(typeof(Bootstrap), "CaptureButton"));
                harmony.Patch(AccessTools.DeclaredMethod(type, "OnDestroy"), null, null, null, new HarmonyMethod(typeof(Bootstrap), "Detach"));
                Debug.Log("[KOE CameraToolsLifetime] One toolbar button; callbacks follow the live controller");
            }
            catch (Exception error) { harmony.UnpatchAll(Id); Debug.LogError("[KOE CameraToolsLifetime] Disabled: " + error.Message); }
            Destroy(this);
        }
        internal static void Prepare(Type type)
        {
            ownerType = type;
            added = AccessTools.Field(type, "hasAddedButton"); fetch = AccessTools.Field(type, "fetch");
            toggle = AccessTools.DeclaredMethod(type, "ToggleGui"); dummy = AccessTools.DeclaredMethod(type, "Dummy");
            if (added == null || !added.IsStatic || added.FieldType != typeof(bool) || fetch == null || !fetch.IsStatic
                || toggle == null || dummy == null || toggle.IsStatic || dummy.IsStatic)
                throw new NotSupportedException("CameraTools toolbar layout changed");
            var fields = new List<FieldInfo>();
            foreach (string name in new[] { "onTrue", "onFalse", "onHover", "onHoverOut", "onEnable", "onDisable" })
            {
                var field = AccessTools.Field(typeof(ApplicationLauncherButton), name);
                if (field == null || field.FieldType != typeof(Callback)) throw new NotSupportedException("Toolbar callback changed: " + name);
                fields.Add(field);
            }
            callbacks = fields.ToArray();
        }
        static void BeforeAdd(object __instance)
        {
            if (button == null) { added.SetValue(null, false); return; }
            Bind(button, __instance); added.SetValue(null, true);
        }
        public static void Track(ApplicationLauncherButton value, object owner) { button = value; Bind(value, owner); }
        internal static void Bind(ApplicationLauncherButton value, object owner)
        {
            for (int i = 0; i < callbacks.Length; i++)
            {
                Delegate kept = null;
                bool replaced = false;
                var current = Delegate.CreateDelegate(typeof(Callback), owner, i < 2 ? toggle : dummy);
                var old = callbacks[i].GetValue(value) as Delegate;
                if (old != null) foreach (var call in old.GetInvocationList())
                {
                    if (call.Target == null || !ownerType.IsInstanceOfType(call.Target)) kept = Delegate.Combine(kept, call);
                    else if (!replaced) { kept = Delegate.Combine(kept, current); replaced = true; }
                }
                callbacks[i].SetValue(value, replaced ? kept : Delegate.Combine(kept, current));
            }
            Rebindings++;
        }
        internal static Exception Detach(object __instance, Exception __exception)
        {
            if (!ReferenceEquals(button, null)) foreach (var field in callbacks)
            {
                var old = field.GetValue(button) as Delegate; Delegate kept = null;
                if (old != null) foreach (var call in old.GetInvocationList())
                    if (!ReferenceEquals(call.Target, __instance)) kept = Delegate.Combine(kept, call);
                field.SetValue(button, kept);
            }
            if (ReferenceEquals(fetch.GetValue(null), __instance)) fetch.SetValue(null, null);
            Detachments++;
            return __exception;
        }
        public static IEnumerable<CodeInstruction> CaptureButton(IEnumerable<CodeInstruction> source)
        {
            var code = new List<CodeInstruction>(source); int count = 0;
            for (int i = 0; i < code.Count; i++)
            {
                var method = code[i].operand as MethodInfo;
                yield return code[i];
                if (method == null || method.DeclaringType != typeof(ApplicationLauncher) || method.Name != "AddModApplication"
                    || method.ReturnType != typeof(ApplicationLauncherButton)) continue;
                if (i+1 == code.Count || code[i+1].opcode != OpCodes.Pop) throw new NotSupportedException("CameraTools no longer discards the created toolbar button");
                var owner = new CodeInstruction(OpCodes.Ldarg_0);
                owner.labels.AddRange(code[i+1].labels); owner.blocks.AddRange(code[i+1].blocks);
                yield return owner;
                yield return new CodeInstruction(OpCodes.Call, AccessTools.Method(typeof(Bootstrap), "Track"));
                i++; count++;
            }
            if (count != 1) throw new NotSupportedException("Expected one CameraTools toolbar creation");
        }
    }
}
