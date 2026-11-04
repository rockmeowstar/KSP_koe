using System;
using System.IO;

namespace CloudLoopFix
{
    // Conservative BC4 occupancy, without truncating fractional UNORM interpolation to byte.
    // Unsupported formats and mip chains must retain the original renderer.
    public sealed class CoverageZeroMap
    {
        readonly int width,height,blocksX,blocksY;
        readonly int[] prefix;
        public int Width{get{return width;}}
        public int Height{get{return height;}}
        public CoverageZeroMap(byte[] dds)
        {
            if(dds==null||dds.Length<128||BitConverter.ToUInt32(dds,0)!=0x20534444)throw new InvalidDataException("Not DDS");
            height=BitConverter.ToInt32(dds,12);width=BitConverter.ToInt32(dds,16);
            uint format=BitConverter.ToUInt32(dds,84);
            if(width<=0||height<=0||width>32768||height>32768||BitConverter.ToInt32(dds,28)>1||BitConverter.ToInt32(dds,24)>1||BitConverter.ToUInt32(dds,112)!=0||(format!=0x55344342&&format!=0x31495441))throw new NotSupportedException("Requires single-mip 2D BC4 UNORM");
            blocksX=(width+3)/4;blocksY=(height+3)/4;
            prefix=Build(dds,128);
        }
        public CoverageZeroMap(int width,int height,byte[] rawBc4)
        {
            if(width<=0||height<=0||width>32768||height>32768)throw new NotSupportedException("Coverage dimensions");
            this.width=width;this.height=height;blocksX=(width+3)/4;blocksY=(height+3)/4;
            prefix=Build(rawBc4,0);
        }
        int[] Build(byte[] bytes,int offset)
        {
            if((long)blocksX*blocksY>4*1024*1024)throw new NotSupportedException("Coverage index budget exceeded");
            if(bytes==null||bytes.Length!=offset+(long)blocksX*blocksY*8)throw new InvalidDataException("BC4 payload length");
            var result=new int[checked((blocksX+1)*(blocksY+1))];
            int stride=blocksX+1;
            for(int y=0;y<blocksY;y++)for(int x=0;x<blocksX;x++){
                int i=(y+1)*stride+x+1;
                result[i]=(Occupied(bytes,offset+(y*blocksX+x)*8)?1:0)+result[i-1]+result[i-stride]-result[i-stride-1];
            }
            return result;
        }
        public static bool Occupied(byte[] bytes,int offset)
        {
            int a=bytes[offset],b=bytes[offset+1];ulong indices=0;
            for(int i=0;i<6;i++)indices|=(ulong)bytes[offset+2+i]<<(8*i);
            if((a|b)==0&&indices==0)return false;
            for(int i=0;i<16;i++){
                int k=(int)(indices&7);indices>>=3;
                if(k==0?a>0:k==1?b>0:a>b?a>0||b>0:k==6?false:k==7?true:a>0||b>0)return true;
            }
            return false;
        }
        public bool EmptyRectangle(double u0,double u1,double v0,double v1)
        {
            double sum=u0+u1+v0+v1;if(double.IsNaN(sum)||double.IsInfinity(sum)||u0<0||u1>1||v0<0||v1>1||u0>u1||v0>v1)return false;
            // A partial query near a wrapping edge needs the opposite edge too.
            // Retain rendering until the caller supplies a split/wrapped certificate.
            if((u0<8.0/width||u1>1-8.0/width)&&(u0!=0||u1!=1))return false;
            if((v0<8.0/height||v1>1-8.0/height)&&(v0!=0||v1!=1))return false;
            // Expand by eight texels for filtering; blocks add another conservative margin.
            int x0=Math.Max(0,(int)Math.Floor(u0*width-8)/4),y0=Math.Max(0,(int)Math.Floor(v0*height-8)/4);
            int x1=Math.Min(blocksX,(int)Math.Ceiling(u1*width+8)/4+1),y1=Math.Min(blocksY,(int)Math.Ceiling(v1*height+8)/4+1);
            int stride=blocksX+1;
            return prefix[y1*stride+x1]-prefix[y0*stride+x1]-prefix[y1*stride+x0]+prefix[y0*stride+x0]==0;
        }
        public bool EmptyCap(double u,double v,double angleDegrees)
        {
            double sum=u+v+angleDegrees;if(double.IsNaN(sum)||double.IsInfinity(sum)||u<0||u>1||v<0||v>1||angleDegrees<0)return false;
            if(angleDegrees>=180)return EmptyRectangle(0,1,0,1);
            double delta=angleDegrees/180;
            double v0=Math.Max(0,v-delta),v1=Math.Min(1,v+delta);
            if(v0==0||v1==1||angleDegrees>=90)return EmptyRectangle(0,1,v0,v1);
            double ratio=Math.Sin(angleDegrees*Math.PI/180)/Math.Sin(v*Math.PI);
            if(ratio>=1)return EmptyRectangle(0,1,v0,v1);
            double du=Math.Asin(ratio)/(2*Math.PI);
            // Unknown wrap/pole cases retain the original renderer.
            return EmptyRectangle(u-du,u+du,v0,v1);
        }
    }
}
