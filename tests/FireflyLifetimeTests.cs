using System;
using System.Collections;
using System.Reflection;
using System.Runtime.Serialization;
using Firefly;
using UnityEngine;

static class FireflyLifetimeTests
{
    const BindingFlags Static = BindingFlags.Static | BindingFlags.NonPublic;
    static int checks;
    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new Exception(message);
    }
    static T Shell<T>() { return (T)FormatterServices.GetUninitializedObject(typeof(T)); }
    public static void Main()
    {
        var type = typeof(FireflyIdleFix.Bootstrap).Assembly.GetType("FireflyIdleFix.DepthLighting", true);
        var cache = (IDictionary)type.GetField("cameras", Static).GetValue(null);
        var materials = (IList)type.GetField("materials", Static).GetValue(null);
        var entry = type.GetNestedType("Entry", BindingFlags.NonPublic);
        var unregister = type.GetMethod("Unregister", Static);
        var other = Shell<Camera>();
        cache.Add(other, Activator.CreateInstance(entry, true));
        for (int i = 0; i < 100; i++)
        {
            var module = Shell<AtmoFxModule>();
            module.fxVessel = Shell<AtmoFxVessel>();
            module.fxVessel.airstreamCamera = Shell<Camera>();
            var owned = Activator.CreateInstance(entry, true);
            entry.GetField("Module").SetValue(owned, module);
            cache.Add(module.fxVessel.airstreamCamera, owned);
            materials.Add(Shell<Material>());
            // The shell has no native pointer, as does a destroyed Unity camera wrapper.
            unregister.Invoke(null, new object[] { module });
            Check(cache.Count == 1 && cache.Contains(other), "Unloading one vessel changed another camera registration");
            Check(materials.Count == 0, "Material scratch list retained an unloaded vessel");
            unregister.Invoke(null, new object[] { module });
            Check(cache.Count == 1, "Repeated unload was not idempotent");
            module.fxVessel = null;
            unregister.Invoke(null, new object[] { module });
            Check(cache.Count == 1, "Missing effect state removed another vessel");
        }
        cache.Clear();
        Console.WriteLine("PASS Firefly lifetime: " + checks + " checks; no Unity runtime rendering exercised");
    }
}
