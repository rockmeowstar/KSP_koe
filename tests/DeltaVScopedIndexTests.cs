using System;
using System.Collections.Generic;
using System.Reflection;
using PropellantRequestFix;

namespace UnityEngine
{
    public class MonoBehaviour { }
    public static class Application { public static event Action quitting { add { } remove { } } }
    public static class Debug { public static void Log(string message) { } }
}
namespace KerbalOverEngineering.Options { public static class Settings { public static bool Enabled(string key) { return true; } } }
namespace HarmonyLib
{
    public class HarmonyMethod : Attribute { public HarmonyMethod(Type type, string name) { } }
    public class PatchInfo { public readonly List<string> Owners = new List<string>(); }
    public class Harmony
    {
        public Harmony(string id) { }
        public static PatchInfo GetPatchInfo(MethodBase method) { return null; }
        public void Patch(MethodBase target, HarmonyMethod prefix = null, HarmonyMethod postfix = null, HarmonyMethod transpiler = null, HarmonyMethod finalizer = null) { }
        public void UnpatchAll(string id) { }
    }
    public static class AccessTools
    {
        public static MethodInfo Method(Type type, string name) { return type.GetMethod(name); }
        public static MethodInfo DeclaredMethod(Type type, string name, Type[] parameters) { return type.GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic, null, parameters, null); }
    }
}
public sealed class Part
{
    public uint persistentId;
    public bool destroyed;
    public static bool operator ==(Part a, Part b) { return ReferenceEquals(a,b) || (ReferenceEquals(a,null) && !ReferenceEquals(b,null) && b.destroyed) || (ReferenceEquals(b,null) && !ReferenceEquals(a,null) && a.destroyed); }
    public static bool operator !=(Part a, Part b) { return !(a==b); }
    public override bool Equals(object o) { return ReferenceEquals(this,o); }
    public override int GetHashCode() { return base.GetHashCode(); }
}
public sealed class DeltaVPartInfo { public Part part; }
public sealed class DeltaVEngineResourcePartInfo { public DeltaVPartInfo resourcePart; }
public sealed class DeltaVEngineInfo { }
public sealed class DeltaVStageInfo { List<DeltaVEngineResourcePartInfo> GetEngineResourceParts(List<DeltaVEngineInfo> e) { return null; } }
public static class DeltaVEngineResourcePartExtensions
{
    public static DeltaVEngineResourcePartInfo Get(List<DeltaVEngineResourcePartInfo> list, DeltaVPartInfo part) { return null; }
    public static bool ContainsPartInfo(List<DeltaVEngineResourcePartInfo> list, DeltaVPartInfo part) { return false; }
}
static class DeltaVScopedIndexTests
{
    static readonly Type Type = typeof(DeltaVLookup);
    static object Enter()
    {
        object[] args = { null }; Type.GetMethod("EnterScope", BindingFlags.NonPublic | BindingFlags.Static).Invoke(null,args); return args[0];
    }
    static void Leave(object scope) { Type.GetMethod("LeaveScope", BindingFlags.NonPublic | BindingFlags.Static).Invoke(null,new[] { scope, null }); }
    static DeltaVEngineResourcePartInfo Entry(uint id, bool dead = false)
    { return new DeltaVEngineResourcePartInfo { resourcePart = new DeltaVPartInfo { part = new Part { persistentId = id, destroyed = dead } } }; }
    static void Check(bool condition, string reason) { if (!condition) throw new Exception(reason); }
    static void Main()
    {
        var reader = Type.GetMethod("VersionReader", BindingFlags.NonPublic | BindingFlags.Static).Invoke(null,null);
        Type.GetField("readVersion", BindingFlags.NonPublic | BindingFlags.Static).SetValue(null,reader);
        var scope = Enter(); var a = Entry(1); var b = Entry(2); var list = new List<DeltaVEngineResourcePartInfo> { a };
        Check(ReferenceEquals(DeltaVLookup.Find(list,a.resourcePart),a), "initial lookup");
        list.Add(b); Check(ReferenceEquals(DeltaVLookup.Find(list,b.resourcePart),b), "append");
        var nested = Enter(); var other = new List<DeltaVEngineResourcePartInfo> { b };
        Check(ReferenceEquals(DeltaVLookup.Find(other,b.resourcePart),b), "nested scope"); Leave(nested);
        Check(ReferenceEquals(DeltaVLookup.Find(list,a.resourcePart),a), "parent scope");
        var duplicate = Entry(1); list.Add(duplicate); a.resourcePart.part.destroyed = true;
        Check(ReferenceEquals(DeltaVLookup.Find(list,duplicate.resourcePart),duplicate), "destroyed duplicate");
        list[1] = Entry(3); Check(DeltaVLookup.Find(list,b.resourcePart) == null, "replacement invalidates index");
        list.RemoveAt(0); Check(ReferenceEquals(DeltaVLookup.Find(list,duplicate.resourcePart),duplicate), "removal fallback");
        Leave(scope);
        Check(ReferenceEquals(DeltaVLookup.Find(list,duplicate.resourcePart),duplicate), "outside scope fallback");
        Console.WriteLine("SCOPED_INDEX_CHECKS_PASSED 8");
    }
}
