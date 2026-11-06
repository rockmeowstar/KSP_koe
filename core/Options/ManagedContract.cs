using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace KerbalOverEngineering.Options
{
    public static class ManagedContract
    {
        static readonly Dictionary<short, OpCode> codes = typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static)
            .Where(f => f.FieldType == typeof(OpCode)).Select(f => (OpCode)f.GetValue(null)).ToDictionary(o => o.Value);

        static string Name(string name)
        {
            return Regex.Replace(name ?? "", @"<>c__DisplayClass\d+_\d+", "<>closure");
        }

        static string TypeName(Type type)
        {
            if (type.IsGenericParameter)
                return (type.DeclaringMethod == null ? "!" : "!!") + type.GenericParameterPosition;
            if (type.HasElementType)
                return TypeName(type.GetElementType()) + (type.IsArray ? "[" + type.GetArrayRank() + "]" : type.IsByRef ? "&" : "*");
            string name = type.Assembly.GetName().Name + ":" + Name(type.IsGenericType ? type.GetGenericTypeDefinition().FullName : type.FullName);
            return type.IsGenericType ? name + "<" + string.Join(",", type.GetGenericArguments().Select(TypeName).ToArray()) + ">" : name;
        }

        static string MemberName(MemberInfo member)
        {
            var type = member as Type;
            if (type != null) return TypeName(type);
            string name = TypeName(member.DeclaringType) + "::" + Name(member.Name);
            var field = member as FieldInfo;
            if (field != null) return name + ":" + TypeName(field.FieldType) + ":" + field.IsStatic;
            var method = member as MethodBase;
            if (method == null) throw new NotSupportedException("Unsupported IL member: " + name);
            var info = method as MethodInfo;
            return name + (method.IsGenericMethod ? "<" + string.Join(",", method.GetGenericArguments().Select(TypeName).ToArray()) + ">" : "")
                + "(" + string.Join(",", method.GetParameters().Select(p => TypeName(p.ParameterType)).ToArray()) + ")"
                + ":" + (info == null ? "ctor" : TypeName(info.ReturnType)) + ":" + method.IsStatic;
        }

        public static string Fingerprint(MethodBase method)
        {
            if (method == null) throw new MissingMethodException("Required mod method is missing");
            var body = method.GetMethodBody();
            if (body == null) throw new NotSupportedException("Required method has no managed body: " + method.Name);
            var text = new StringBuilder(MemberName(method)).Append('|').Append(body.InitLocals);
            foreach (var local in body.LocalVariables)
                text.Append('|').Append(TypeName(local.LocalType)).Append(':').Append(local.IsPinned);
            foreach (var clause in body.ExceptionHandlingClauses)
            {
                text.Append('|').Append(clause.Flags).Append(':').Append(clause.TryOffset).Append(':').Append(clause.TryLength)
                    .Append(':').Append(clause.HandlerOffset).Append(':').Append(clause.HandlerLength);
                if (clause.Flags == ExceptionHandlingClauseOptions.Clause) text.Append(':').Append(TypeName(clause.CatchType));
                if (clause.Flags == ExceptionHandlingClauseOptions.Filter) text.Append(':').Append(clause.FilterOffset);
            }
            var bytes = body.GetILAsByteArray();
            Type[] typeArgs = method.DeclaringType.IsGenericType ? method.DeclaringType.GetGenericArguments() : null;
            Type[] methodArgs = method.IsGenericMethod ? method.GetGenericArguments() : null;
            for (int at = 0; at < bytes.Length;)
            {
                short value = bytes[at++];
                if (value == 0xfe) value = (short)(0xfe00 | bytes[at++]);
                OpCode op = codes[value];
                text.Append('|').Append(op.Name).Append(':');
                int size;
                switch (op.OperandType)
                {
                    case OperandType.InlineNone: size = 0; break;
                    case OperandType.ShortInlineBrTarget:
                    case OperandType.ShortInlineI:
                    case OperandType.ShortInlineVar: size = 1; break;
                    case OperandType.InlineVar: size = 2; break;
                    case OperandType.InlineI8:
                    case OperandType.InlineR: size = 8; break;
                    case OperandType.InlineSwitch: size = checked(4 + 4 * BitConverter.ToInt32(bytes, at)); break;
                    case OperandType.InlineSig: throw new NotSupportedException("Indirect IL signatures require a separate contract");
                    default: size = 4; break;
                }
                if (size < 0 || at > bytes.Length - size) throw new BadImageFormatException("Truncated IL operand");
                switch (op.OperandType)
                {
                    case OperandType.InlineField:
                    case OperandType.InlineMethod:
                    case OperandType.InlineType:
                    case OperandType.InlineTok:
                        text.Append(MemberName(method.Module.ResolveMember(BitConverter.ToInt32(bytes, at), typeArgs, methodArgs)));
                        break;
                    case OperandType.InlineString:
                        string literal = method.Module.ResolveString(BitConverter.ToInt32(bytes, at));
                        text.Append(Convert.ToBase64String(Encoding.UTF8.GetBytes(literal)));
                        break;
                    default:
                        for (int i = 0; i < size; i++) text.Append(bytes[at + i].ToString("x2", CultureInfo.InvariantCulture));
                        break;
                }
                at += size;
            }
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(text.ToString()))).Replace("-", "").ToLowerInvariant();
        }

        public static void Require(MethodBase method, string expected)
        {
            if (Fingerprint(method) != expected)
                throw new NotSupportedException("Mod algorithm changed: " + method.DeclaringType.FullName + "." + method.Name);
        }

        public static MethodInfo Method(Assembly assembly, string type, string name, params Type[] args)
        {
            var target = assembly == null ? null : assembly.GetType(type, false);
            return target == null ? null : target.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static, null, args, null);
        }

        public static string ParallaxProblem(Assembly assembly, string feature)
        {
            if (assembly == null) return "ParallaxContinued is missing";
            // AI-generated comment: Upstream moved DDS uploads to KSPTextureLoader and removed RGB expansion.
            // Detect that API transition without pinning an assembly identity.
            bool externalLoader = assembly.GetReferencedAssemblies().Any(a => a.Name == "KSPTextureLoader");
            if (externalLoader)
                return "Installed Parallax uses the upstream loading pipeline; KOE " + feature + " is unnecessary or unsupported";
            switch (feature)
            {
                case "GpuSkyboxCopy":
                    return Method(assembly, "Parallax.Scaled_System.SkyboxControl", "ExtractSkyboxFrom", typeof(UnityEngine.GameObject)) != null ? null : "Parallax skybox API unavailable";
                case "DdsDirectUpload":
                    return Method(assembly, "Parallax.TextureLoader", "LoadDDSTexture", typeof(string), typeof(bool), typeof(bool)) != null ? null : "Parallax DDS API unavailable";
                case "BiomeRgbPreparation":
                case "LaunchTextureRetention":
                    return Method(assembly, "Parallax.ScatterManager", "DominantBodyLoaded", typeof(string)) != null ? null : "Parallax scatter API unavailable";
            }
            return "Unknown Parallax feature";
        }

        public static string EveProblem(Assembly assembly, Assembly utils, string feature)
        {
            if (assembly == null) return "Atmosphere is missing";
            if (feature == "TexturePrefetch" || feature == "DdsDirectUpload")
            {
                return EveDdsLoader(utils) == null ? "EVE DDS loader API unavailable" : null;
            }
            string type = feature == "MenuInitScheduler" ? "Atmosphere.CelestialBodyCloudsHandler"
                : feature == "CurveBakeHoist" || feature == "CoverageZeroCull" ? "Atmosphere.CloudsRaymarchedVolume"
                : "Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer";
            return assembly.GetType(type, false) == null ? "EVE API unavailable for " + feature : null;
        }

        public static MethodInfo EveDdsLoader(Assembly utils)
        {
            var converter = utils == null ? null : utils.GetType("Utils.TextureConverter", false);
            if (converter == null) return null;
            string[] parameters = { "GameDatabase+TextureInfo", "System.Boolean", "UnityEngine.Vector2", "System.String", "System.Boolean" };
            return converter.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                .SingleOrDefault(m => m.Name == "DDSToTexture" && m.ReturnType == typeof(void)
                    && m.GetParameters().Select(p => p.ParameterType.FullName).SequenceEqual(parameters));
        }

        public static string IndexedBiomeProblem(Type provider)
        {
            if (provider == null || provider.FullName != "Kopernicus.Components.KopernicusCBAttributeMapSO"
                || provider.Assembly.GetName().Name != "Kopernicus") return "Kopernicus indexed biome provider is missing";
            var flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly;
            if (provider.GetMethods(flags).Any(m => m.Name == "CompileRGB" || m.Name == "CompileRGBA"))
                return "Kopernicus owns optimized biome texture compilation";
            return null;
        }
    }
}
