using System;

namespace CloudLoopFix
{
    // Scene-independent geometric kernel; the caller supplies a verified coverage certificate.
    public static class CoverageVisibility
    {
        public static double OccluderRadius(double bodyRadius, bool hasOcean, double terrainMinimumRadius)
        {
            if (double.IsNaN(bodyRadius) || double.IsInfinity(bodyRadius) || bodyRadius <= 0) return double.NaN;
            bool terrainKnown = !double.IsNaN(terrainMinimumRadius) && !double.IsInfinity(terrainMinimumRadius) && terrainMinimumRadius > 0;
            if (!hasOcean && !terrainKnown) return double.NaN;
            // Use the actual sea-level radius or the terrain's minimum bound.
            // This relative conservative inset is independent of planet size.
            double bound = terrainKnown ? Math.Min(bodyRadius, terrainMinimumRadius) : bodyRadius;
            return bound * .95;
        }
        public static double VisibleCapDegrees(double cameraRadius,double outerRadius,double occluderRadius)
        {
            double sum=cameraRadius+outerRadius+occluderRadius;
            if(double.IsNaN(sum)||double.IsInfinity(sum)||occluderRadius<=0||cameraRadius<=occluderRadius||outerRadius<=occluderRadius)return double.NaN;
            return (Math.Acos(occluderRadius/cameraRadius)+Math.Acos(occluderRadius/outerRadius))*180/Math.PI;
        }
        public static bool HiddenInLatitudeBand(double cameraRadius,double outerRadius,double occluderRadius,double latitudeDegrees,double emptyMinLatitude,double emptyMaxLatitude,double marginDegrees)
        {
            double sum=cameraRadius+outerRadius+occluderRadius+latitudeDegrees+emptyMinLatitude+emptyMaxLatitude+marginDegrees;
            if(double.IsNaN(sum)||double.IsInfinity(sum)||marginDegrees<0)return false;
            if(occluderRadius<=0||cameraRadius<=occluderRadius||outerRadius<=occluderRadius||emptyMinLatitude>=emptyMaxLatitude)return false;
            double cap=VisibleCapDegrees(cameraRadius,outerRadius,occluderRadius)+marginDegrees;
            return latitudeDegrees-cap>emptyMinLatitude&&latitudeDegrees+cap<emptyMaxLatitude;
        }
    }
}
