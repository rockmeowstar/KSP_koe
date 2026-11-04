using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Threading.Tasks;
using HarmonyLib;
using UnityEngine;

namespace CloudLoopFix
{
    // Shared runtime: production and diagnostics compile the same visibility implementation.
#if KOE_COVERAGE_PRODUCTION
    [KSPAddon(KSPAddon.Startup.Instantly,true)]
#endif
    public class CoverageCull:MonoBehaviour
    {
        static FieldInfo coverage,coveragePath,rotation,body,outer,boundsField,boundLayer;
        static FieldInfo mapType,mapMask,mapTexture,flowMap,detailTex,painterTiles,painterMasks,mainMaterial,reflectionMaterial,isReflection;
        static Func<Vector3,Vector2> getUV;
        static bool wide;
        static readonly List<int> remove=new List<int>();
        static readonly Dictionary<object,bool> decisions=new Dictionary<object,bool>();
        sealed class Binding{public object Wrapper;public Texture2D Texture;}
        static readonly ConditionalWeakTable<Material,Binding> bindings=new ConditionalWeakTable<Material,Binding>();
        sealed class Candidate{public bool Eligible;public string Path;public object Wrapper,Type,Mask;public Texture2D Texture;public uint UpdateCount;public Task<CoverageZeroMap> Map;public CoverageZeroMap Proof;public bool Reported;}
        static readonly ConditionalWeakTable<object,Candidate> candidates=new ConditionalWeakTable<object,Candidate>();
        static long checks,culled;
        public static Action<string,string> Reporter;
        static void Emit(string kind,string message){if(Reporter!=null)Reporter(kind,message);else Debug.Log("[CoverageCullFix] "+kind+": "+message);}
        Harmony harmony;
        public void Awake()
        {
            if (!KerbalOverEngineering.Options.Settings.Enabled("EVE.CoverageZeroCull")) { Destroy(this); return; }
#if KOE_COVERAGE_PRODUCTION
            wide=true;if(Environment.GetEnvironmentVariable("KOE_COVERAGE_CULL")=="0"){Destroy(this);return;}
#else
            wide=Environment.GetEnvironmentVariable("KOE_EVE_COVERAGE_CULL")=="1";
            if(!wide&&Environment.GetEnvironmentVariable("KOE_EVE_AURORA_CULL")!="1"){Destroy(this);return;}
#endif
            harmony=new Harmony("local.kerbaloverengineering.eve-coverage-cull");
            try{
                if(!wide){
                    string path=Path.Combine(KSPUtil.ApplicationRootPath,@"GameData\SPVE\PluginData\auroraCoverage.dds");
                    string hash;using(var sha=SHA256.Create())using(var stream=File.OpenRead(path))hash=BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","").ToLowerInvariant();
                    if(hash!="aad00fe8d23112e4af49fbc9e4b9e3192da6ead4b2ef936815264bf061dce366")throw new NotSupportedException("Unknown aurora map");
                    if(!new CoverageZeroMap(File.ReadAllBytes(path)).EmptyRectangle(0,1,(90-57)/180.0,(90+57)/180.0))throw new NotSupportedException("Zero coverage certificate failed");
                }
                var assembly=AppDomain.CurrentDomain.GetAssemblies().Single(a=>a.GetName().Name=="Atmosphere");
                if(assembly.ManifestModule.ModuleVersionId!=new Guid("8150e218-893c-4b2c-a852-b674a3006cfd"))throw new NotSupportedException("Unknown EVE");
                var volume=assembly.GetType("Atmosphere.CloudsRaymarchedVolume",true);
                coverage=AccessTools.Field(volume,"coverageMap");rotation=AccessTools.Field(volume,"cloudRotationMatrix");
                body=AccessTools.Field(volume,"parentCelestialBody");outer=AccessTools.Field(volume,"outerSphereRadius");
                coveragePath=AccessTools.Field(coverage.FieldType,"value");
                if(new[]{coverage,rotation,body,outer,coveragePath}.Any(f=>f==null))throw new MissingFieldException("Aurora visibility fields");
                if(wide){
                    Type wrapper=coverage.FieldType;
                    if(wrapper.Assembly.ManifestModule.ModuleVersionId!=new Guid("752c32c2-9c32-40be-bad9-8a27f80a0a26"))throw new NotSupportedException("Unknown EVE UV mapping");
                    mapType=AccessTools.Field(wrapper,"type");mapMask=AccessTools.Field(wrapper,"alphaMask");mapTexture=AccessTools.Field(wrapper,"textureValue");
                    flowMap=AccessTools.Field(volume,"flowMap");detailTex=AccessTools.Field(volume,"useDetailTex");painterTiles=AccessTools.Field(volume,"painterTiles");painterMasks=AccessTools.Field(volume,"painterTileMasks");
                    mainMaterial=AccessTools.Field(volume,"raymarchedCloudMaterial");reflectionMaterial=AccessTools.Field(volume,"reflectionProbeRaymarchedCloudMaterial");
                    if(new[]{mapType,mapMask,mapTexture,flowMap,detailTex,painterTiles,painterMasks,mainMaterial,reflectionMaterial}.Any(f=>f==null))throw new MissingFieldException("Coverage proof fields");
                    getUV=(Func<Vector3,Vector2>)Delegate.CreateDelegate(typeof(Func<Vector3,Vector2>),AccessTools.DeclaredMethod(wrapper,"GetEquirectangularUV",new[]{typeof(Vector3)}));
                }
                var renderer=assembly.GetType("Atmosphere.DeferredRaymarchedVolumetricCloudsRenderer",true);
                boundsField=AccessTools.Field(renderer,"volumesBounds");boundLayer=AccessTools.Field(boundsField.FieldType.GetGenericArguments()[0],"layer");
                isReflection=AccessTools.Field(renderer,"reflectionProbeCamera");if(isReflection==null)throw new MissingFieldException("Reflection camera guard");
                if(boundLayer==null)throw new MissingFieldException("Cloud bounds layer");
                if(wide){
                    var binder=AccessTools.DeclaredMethod(coverage.FieldType,"ApplyTexture");var parameters=binder.GetParameters();
                    if(parameters.Length<2||parameters[0].ParameterType!=typeof(Material)||parameters[1].ParameterType!=typeof(string))throw new NotSupportedException("Texture binding layout");
                    harmony.Patch(binder,postfix:new HarmonyMethod(typeof(CoverageCull),"AfterBinding"));
                }
                harmony.Patch(AccessTools.DeclaredMethod(renderer,"OnPreRender"),prefix:new HarmonyMethod(typeof(CoverageCull),"BeforeRender"));
                DontDestroyOnLoad(gameObject);Emit("COVERAGE_CULL_MODE","visibility across scenes/cameras; wide="+wide+"; exact BC4 zero coverage; planet occlusion bound 0.95R; three-degree margin; lighting list unchanged; unsupported cases retain original rendering");
            }catch(Exception ex){harmony.UnpatchAll(harmony.Id);harmony=null;Emit("COVERAGE_CULL_DISABLED",ex.GetType().Name+": "+ex.Message);Destroy(this);}
        }
        public static bool Hidden(double cameraRadius,double outerRadius,double occluderRadius,double latitudeDegrees)
        {
            return CoverageVisibility.HiddenInLatitudeBand(cameraRadius,outerRadius,occluderRadius,latitudeDegrees,-57,57,3);
        }
        public static void AfterBinding(object __instance,Material __0,string __1)
        {
            if(__1!="CloudCoverage"||__0==null)return;
            var texture=mapTexture.GetValue(__instance) as Texture2D;
            bindings.Remove(__0);if(texture!=null)bindings.Add(__0,new Binding{Wrapper=__instance,Texture=texture});
        }
        public static bool ShouldCull(object volume,Camera camera,bool reflection)
        {
            Candidate candidate;if(!candidates.TryGetValue(volume,out candidate)){
                object wrapper=coverage.GetValue(volume);string source=wrapper==null?null:(string)coveragePath.GetValue(wrapper);
                candidate=new Candidate{Eligible=source=="SPVE/PluginData/auroraCoverage",Path=source,Wrapper=wrapper};
                if(wide){
                    candidate.Eligible=false;
                    if(wrapper!=null&&mapType.GetValue(wrapper).ToString()=="AlphaMap"&&mapMask.GetValue(wrapper).ToString()=="ALPHAMAP_R"&&flowMap.GetValue(volume)==null&&!(bool)detailTex.GetValue(volume)&&!HasItems(painterTiles.GetValue(volume))&&!HasItems(painterMasks.GetValue(volume))){
                        candidate.Type=mapType.GetValue(wrapper);candidate.Mask=mapMask.GetValue(wrapper);
                        candidate.Texture=mapTexture.GetValue(wrapper) as Texture2D;
                        if(candidate.Texture!=null&&candidate.Texture.format==TextureFormat.BC4&&candidate.Texture.isReadable&&candidate.Texture.mipmapCount==1&&candidate.Texture.anisoLevel<=1){
                            int width=candidate.Texture.width,height=candidate.Texture.height;
                            if((long)((width+3)/4)*((height+3)/4)<=4*1024*1024){
                                candidate.UpdateCount=candidate.Texture.updateCount;
                                byte[] raw;
                                try{raw=candidate.Texture.GetRawTextureData();}catch(UnityException){raw=null;}
                                if(raw!=null&&candidate.UpdateCount==candidate.Texture.updateCount){
                                    candidate.Eligible=true;candidate.Map=Task.Run(()=>{try{return new CoverageZeroMap(width,height,raw);}catch(IOException){return null;}catch(NotSupportedException){return null;}});
                                }
                            }
                        }
                    }
                }
                if(wide)Emit("COVERAGE_CULL_SOURCE","path="+source+"; eligible="+candidate.Eligible+"; type="+(wrapper==null?"none":mapType.GetValue(wrapper).ToString())+"; mask="+(wrapper==null?"none":mapMask.GetValue(wrapper).ToString())+"; texture="+(candidate.Texture==null?"none":candidate.Texture.width+"x"+candidate.Texture.height+"; mips="+candidate.Texture.mipmapCount+"; aniso="+candidate.Texture.anisoLevel+"; format="+candidate.Texture.format+"; readable="+candidate.Texture.isReadable));
                candidates.Add(volume,candidate);
            }
            if(!candidate.Eligible)return false;
            object currentCoverage=coverage.GetValue(volume);
            if(currentCoverage==null||!ReferenceEquals(currentCoverage,candidate.Wrapper)||(string)coveragePath.GetValue(currentCoverage)!=candidate.Path)return false;checks++;
            CoverageZeroMap proof=null;
            if(wide){
                if(!candidate.Reported){
                    if(!candidate.Map.IsCompleted)return false;
                    candidate.Proof=candidate.Map.IsFaulted||candidate.Map.IsCanceled?null:candidate.Map.Result;candidate.Map=null;candidate.Reported=true;
                    Emit("COVERAGE_CULL_MAP","path="+candidate.Path+"; ready="+(candidate.Proof!=null)+"; source=live BC4 texture; no GPU readback");
                }
                proof=candidate.Proof;
                if(proof==null||candidate.Texture==null||!ReferenceEquals(mapTexture.GetValue(currentCoverage),candidate.Texture)||!Equals(mapType.GetValue(currentCoverage),candidate.Type)||!Equals(mapMask.GetValue(currentCoverage),candidate.Mask)||candidate.Texture.updateCount!=candidate.UpdateCount||candidate.Texture.format!=TextureFormat.BC4||candidate.Texture.width!=proof.Width||candidate.Texture.height!=proof.Height||candidate.Texture.mipmapCount!=1||candidate.Texture.anisoLevel>1)return false;
                var material=(Material)(reflection?reflectionMaterial:mainMaterial).GetValue(volume);
                Binding binding;
                if(material==null||!bindings.TryGetValue(material,out binding)||!ReferenceEquals(binding.Wrapper,currentCoverage)||binding.Texture!=candidate.Texture)return false;
                // The original setter is authoritative: these shader uniforms cannot be
                // read using Material.GetTexture, even when HasProperty returns true.
                if(flowMap.GetValue(volume)!=null||(bool)detailTex.GetValue(volume)||HasItems(painterTiles.GetValue(volume))||HasItems(painterMasks.GetValue(volume)))return false;
            }
            var planet=(CelestialBody)body.GetValue(volume);
            if(planet==null||planet.bodyName!="Kerbin"||!planet.ocean||planet.Radius!=600000)return false;
            var matrix=(Matrix4x4)rotation.GetValue(volume);
            // A spherical cap stays spherical only under a rigid transform.
            Vector3 x=matrix.GetColumn(0),y=matrix.GetColumn(1),z=matrix.GetColumn(2);
            if(Mathf.Abs(x.sqrMagnitude-1)>0.0001f||Mathf.Abs(y.sqrMagnitude-1)>0.0001f||Mathf.Abs(z.sqrMagnitude-1)>0.0001f||Mathf.Abs(Vector3.Dot(x,y))>0.0001f||Mathf.Abs(Vector3.Dot(x,z))>0.0001f||Mathf.Abs(Vector3.Dot(y,z))>0.0001f)return false;
            Vector3 mapped=matrix.MultiplyPoint(camera.transform.position);double radius=(camera.transform.position-planet.transform.position).magnitude;
            if(mapped.sqrMagnitude<=0||Math.Abs(mapped.magnitude-radius)>radius*.0001)return false;
            double latitude=Math.Asin(Math.Max(-1,Math.Min(1,mapped.normalized.y)))*180/Math.PI;
            if(wide){Vector2 uv=getUV(mapped.normalized);return proof.EmptyCap(uv.x,uv.y,CoverageVisibility.VisibleCapDegrees(radius,(float)outer.GetValue(volume),planet.Radius*.95)+3);}
            return Hidden(radius,(float)outer.GetValue(volume),planet.Radius*.95,latitude);
        }
        static bool HasItems(object value){var list=value as ICollection;return list!=null&&list.Count>0;}
        public static void BeforeRender(object __instance)
        {
            Camera camera=Camera.current;if(camera==null)return;
            var bounds=(IList)boundsField.GetValue(__instance);remove.Clear();decisions.Clear();
            bool reflection=(bool)isReflection.GetValue(__instance);
            for(int i=0;i<bounds.Count;i++){
                object layer=boundLayer.GetValue(bounds[i]);bool hidden;
                if(!decisions.TryGetValue(layer,out hidden)){hidden=ShouldCull(layer,camera,reflection);decisions.Add(layer,hidden);}
                if(hidden)remove.Add(i);
            }
            // Keep at least one interval: the original renderer doesn't initialize empty ray output.
            // volumesAdded stays intact for LightVolume, global radii, fade and reconstruction inputs.
            culled+=ApplyRemovals(bounds,remove)/2;decisions.Clear();
        }
        public static int ApplyRemovals(IList bounds,List<int> indices)
        {
            if(indices.Count==bounds.Count)return 0;
            for(int i=indices.Count-1;i>=0;i--)bounds.RemoveAt(indices[i]);return indices.Count;
        }
        public void OnApplicationQuit(){if(harmony!=null)Emit("COVERAGE_CULL_COUNTERS","checks="+checks+"; culled="+culled);}
        public void OnDestroy(){if(harmony!=null)harmony.UnpatchAll(harmony.Id);}
    }
}
