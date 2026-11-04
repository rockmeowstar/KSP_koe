using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace FireflyIdleFix
{
    // Compare raw bits: signed zero and NaN payloads must not be coalesced.
    internal struct GlobalValue : IEquatable<GlobalValue>
    {
        [StructLayout(LayoutKind.Explicit)]struct Word { [FieldOffset(0)]public float Float;[FieldOffset(0)]public uint Bits; }
        public uint X,Y,Z,W;
        static uint Bits(float value){return new Word{Float=value}.Bits;}
        public static GlobalValue From(float x,float y,float z,float w){return new GlobalValue{X=Bits(x),Y=Bits(y),Z=Bits(z),W=Bits(w)};}
        public bool Equals(GlobalValue other){return X==other.X&&Y==other.Y&&Z==other.Z&&W==other.W;}
    }
    internal sealed class GlobalCommandCache
    {
        readonly Dictionary<int,GlobalValue> values=new Dictionary<int,GlobalValue>();
        public bool ShouldEmit(int id,GlobalValue value)
        {
            GlobalValue prior;if(values.TryGetValue(id,out prior)&&prior.Equals(value))return false;
            values[id]=value;return true;
        }
    }
}
