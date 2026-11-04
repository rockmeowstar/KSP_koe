using System;
using CloudLoopFix;

static class CoverageGeometryTests
{
    static int checks;
    static void Check(bool condition, string message) { checks++; if (!condition) throw new Exception(message); }
    static int Main()
    {
        try
        {
            foreach (double size in new[] { 1000d, 600000d, 6371000d, 70000000d })
            {
                double radius = CoverageVisibility.OccluderRadius(size, true, double.NaN);
                Check(radius == size * .95, "Radius derived from actual body size");
                double cap = CoverageVisibility.VisibleCapDegrees(size * 1.01, size * 1.02, radius);
                Check(Math.Abs(cap - CoverageVisibility.VisibleCapDegrees(1.01, 1.02, .95)) < 1e-10, "Scale-invariant angular bound");
                double deepTerrain = size * .6;
                Check(CoverageVisibility.OccluderRadius(size, false, deepTerrain) <= deepTerrain, "Deep terrain never over-occluded");
                Check(CoverageVisibility.OccluderRadius(size, true, deepTerrain) <= deepTerrain, "Terrain bound remains conservative on ocean worlds");
                Check(double.IsNaN(CoverageVisibility.OccluderRadius(size, false, double.NaN)), "Unknown solid surface retains original rendering");
                Check(double.IsNaN(CoverageVisibility.VisibleCapDegrees(radius, size * 1.02, radius)), "Camera inside conservative surface does not cull");
            }
            foreach (double bad in new[] { 0d, -1d, double.NaN, double.PositiveInfinity, double.NegativeInfinity })
                Check(double.IsNaN(CoverageVisibility.OccluderRadius(bad, true, 1)), "Invalid body dimensions rejected");
            Check(!CoverageVisibility.HiddenInLatitudeBand(1, 2, 1, 0, -57, 57, 3), "Invalid camera geometry cannot suppress clouds");
            // Independent line/sphere intersection oracle: a point outside the
            // predicted visible cap must be hidden by the conservative sphere.
            var random = new Random(4321);
            for (int i = 0; i < 10000; i++)
            {
                double camera = 1 + random.NextDouble() * 20, cloud = 1 + random.NextDouble() * 5;
                double angle = random.NextDouble() * Math.PI;
                double cap = CoverageVisibility.VisibleCapDegrees(camera, cloud, 1) * Math.PI / 180;
                if (angle <= cap + 1e-9) continue;
                double px = cloud * Math.Cos(angle), py = cloud * Math.Sin(angle);
                double dx = px - camera, dy = py;
                double along = Math.Max(0, Math.Min(1, -camera * dx / (dx * dx + dy * dy)));
                double x = camera + along * dx, y = along * dy;
                Check(x * x + y * y <= 1 + 1e-9, "Angular cull agrees with independent ray occlusion");
            }
            Console.WriteLine("COVERAGE_GEOMETRY_PASS checks=" + checks + "; multiple body scales; no renderer executed"); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
