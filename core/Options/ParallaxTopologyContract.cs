using System;
using System.Linq;
using System.Reflection;

namespace KerbalOverEngineering.Options
{
    public static class ParallaxTopologyContract
    {
        static readonly string[] Controller = {
            "Initialize=d908f73fe8fe66a4c0e3fd2995f0d80bca7ad8486878cbef738850da8bd24616",
            "InitializeSourceData=bf5e4e043d2ef6a846a9581c2e3ac104c6588b960081d003598bd81656e3a135",
            "DisposeSourceData=0348d7607f9b8d4ad8ffe3b75186b3919e102c1f7719537db99c388a1cc0153d",
            "RefreshSourceData=5060bf7fa2a2db683acdfea212adf2fad30950c91bb105de2f5864fd7617796b",
            "CreateTriangles=c25a5d44ad9ba08be465e6fd0bdfe6b7136638e9181d24575cf61fceaf8ded6e",
            "Update=af18cddbff1d10ee57cb77bc420cf354ff08167a3bd90c1af35ed71ea81e4444",
            "UpdateLocalFrustumPlanes=a87cc5db4ca3de8e7320cd389c327b11fc82fb2e207951e1eeb8bc6ccf21ea96",
            "RequiresDispatch=943ea555b979d297c12cc300597afdae3e11fea9b662aea67b679e75b22da31b",
            "UpdateLevelThresholds=d69ee6ff9a38e0660cf9c804835264e41f650939546309ae0da800f102d71891",
            "DispatchSubdivision=0714c26118bf882c74b30af7c34f7296d05583c6f53a39af5255e7d057419c46",
            "BuildMesh=87006cba0b5e0942c3ff9a48b430af81efff17d41aeb8542c1e1c1333d086893",
            "ResetMesh=ce44c017f0eb7316cec6eecb03ece5a63d3bf357045829c7d15b9684e6c2fa6d",
            "SetVertexData=cac0d1071bf90157124680b53489ef4b83219e0bb8fad06db72cb4532b6613a2",
            "SetIndexData=e3bad4ac42efd4e7ab05319f5d753483a41f2b3f23bd46fbd7a4ac71385a7c66",
            "CompleteAllJobs=79bb72cfcac8dcef72f5393ef642442835a279924f86ace80934c073109d4e66",
            "Cleanup=0810a1e22db3e3f6f169f5c7edc2b56014d1f522454f9d2da683b6a412536b74"
        };
        static readonly string[] Triangle = {
            "Subdivide=3170ef007b6617917a80fb7dfe1a75a6b25871417d74e73cee661dc1b3ac3571",
            "GetSubdivisionLevel=f1f43e22568bb664e6688ea1fd18dc9e911cb85d05089be9dc6ec2f0fa9df21f",
            "AreTwoVertsOutOfRange=15d392c738ef64b908b682c83a472c0b662d83950ef3c33f635da6abc6dbd2f6",
            "IsOneVertexOutOfRange=c7f20520c5b6ac34c8451b360681cfe8745e690c2c987755ae41e009dbb2189d",
            "AllThreeOutOfRange=0d79a24200701b3061c732dc6daf5a0e5e59ea5cd39bd57f197ed3dd05401a9c",
            "IsTriangleCorrupt=555df35474a7f2a32662d67e475a9862601cbe0190b722b34aeb3408c18a4fa5",
            "GetVertexBetween=ddba739d3a436e0ba2510d26f0275af6d89faeae24d1d2b4aaf8209228871bd9",
            "GetNormalBetween=355dcd207d986037c115068a0fb78f69b531495abbaf5a6e375b06bc77e3c3f2",
            "GetColorBetween=19d35febd588aff6d53312794b3d3c932bc5601c36dd8ca15b6ba4128109eb17",
            "GetFloatBetween=5cc3264ca40a9eb9971213cf9894d9cd3d68f2cc76ce8dc1cae08749358b8bac"
        };

        static void Require(Assembly assembly, string name, string[] entries)
        {
            var type = assembly.GetType(name, true);
            var methods = type.GetMethods(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
            foreach (string entry in entries) {
                string[] parts = entry.Split('=');
                ManagedContract.Require(methods.Single(m => m.Name == parts[0]), parts[1]);
            }
        }

        public static string Check(Assembly assembly)
        {
            if (assembly == null) return "ParallaxContinued is missing";
            try {
                Require(assembly, "Parallax.JobifiedSubdivision", Controller);
                Require(assembly, "Parallax.SubdividableTriangle", Triangle);
                Require(assembly, "Parallax.ParallaxPlane", new[] { "GetSide=0ebe64377c29b043e3560bfc7e862295c16822b35725c5a190ee4b9951dbea87" });
                return null;
            }
            catch (Exception error) { return "Parallax topology API differs: " + error.Message; }
        }
    }
}
