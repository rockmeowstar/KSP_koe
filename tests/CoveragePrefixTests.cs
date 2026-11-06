using System;
using System.Diagnostics;
using System.Linq;
using CloudLoopFix;

static class CoveragePrefixTests
{
    static int checks;
    static void Check(bool ok, string message)
    {
        checks++;
        if (!ok) throw new Exception(message);
    }

    static bool ReferenceOccupied(byte[] input, int at)
    {
        int a = input[at], b = input[at + 1];
        ulong indices = 0;
        for (int i = 0; i < 6; i++) indices |= (ulong)input[at + i + 2] << (i * 8);
        for (int i = 0; i < 16; i++, indices >>= 3)
        {
            int k = (int)(indices & 7);
            int numerator = k == 0 ? a : k == 1 ? b
                : a > b ? (8 - k) * a + (k - 1) * b
                : k == 6 ? 0 : k == 7 ? 255 : (6 - k) * a + (k - 1) * b;
            if (numerator > 0) return true;
        }
        return false;
    }

    static void ReferencePrefix(byte[] input, int offset, int width, int height, int[] output)
    {
        Array.Clear(output, 0, output.Length);
        int stride = width + 1;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                int i = (y + 1) * stride + x + 1;
                output[i] = (ReferenceOccupied(input, offset + (y * width + x) * 8) ? 1 : 0)
                    + output[i - 1] + output[i - stride] - output[i - stride - 1];
            }
    }

    static bool OriginalOccupied(byte[] bytes, int offset)
    {
        int a = bytes[offset], b = bytes[offset + 1];
        ulong indices = 0;
        for (int i = 0; i < 6; i++) indices |= (ulong)bytes[offset + 2 + i] << (8 * i);
        if ((a | b) == 0 && indices == 0) return false;
        for (int i = 0; i < 16; i++)
        {
            int k = (int)(indices & 7); indices >>= 3;
            if (k == 0 ? a > 0 : k == 1 ? b > 0 : a > b ? a > 0 || b > 0 : k == 6 ? false : k == 7 ? true : a > 0 || b > 0) return true;
        }
        return false;
    }

    static void OriginalPrefix(byte[] input, int width, int height, int[] output)
    {
        Array.Clear(output, 0, output.Length);
        int stride = width + 1;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                int i = (y + 1) * stride + x + 1;
                output[i] = (OriginalOccupied(input, (y * width + x) * 8) ? 1 : 0)
                    + output[i - 1] + output[i - stride] - output[i - stride - 1];
            }
    }

    static void Compare(byte[] bytes, int offset, int width, int height, bool native)
    {
        int count = (width + 1) * (height + 1);
        var expected = new int[count];
        var actual = Enumerable.Repeat(-93, count).ToArray();
        ReferencePrefix(bytes, offset, width, height, expected);
        CoverageZeroMap.BuildManaged(bytes, offset, width, height, actual);
        Check(expected.SequenceEqual(actual), "Managed integral image differs from decoder oracle");
        if (offset == 0)
        {
            var map = new CoverageZeroMap(width * 4, height * 4, bytes);
            var built = (int[])typeof(CoverageZeroMap).GetField("prefix", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(map);
            Check(expected.SequenceEqual(built), "Coverage map constructor must use an exact integral image");
        }
        for (int i = offset; i < bytes.Length; i += 8)
            Check(CoverageZeroMap.Occupied(bytes, i) == ReferenceOccupied(bytes, i), "BC4 occupancy differs from decoder oracle");
        if (native)
        {
            Array.Clear(actual, 0, actual.Length);
            Check(CoverageNative.TryBuild(bytes, offset, width, height, actual), "Native batch rejected valid map");
            Check(expected.SequenceEqual(actual), "Rust integral image differs from decoder oracle");
        }
    }

    static double Time(Action action)
    {
        action(); action();
        var samples = new double[7];
        for (int i = 0; i < samples.Length; i++)
        {
            var timer = Stopwatch.StartNew(); action(); timer.Stop(); samples[i] = timer.Elapsed.TotalMilliseconds;
        }
        Array.Sort(samples);
        return samples[3];
    }

    static int Main(string[] args)
    {
        try
        {
            bool native = args.Length != 0;
            if (native) KerbalOverEngineering.Options.NativeAssets.Folder = args[0];
            Check(CoverageNative.Available == native, "Native availability / portable fallback");
            for (int k = 0; k < 8; k++)
            {
                var bytes = new byte[65536 * 8];
                for (int endpoints = 0; endpoints < 65536; endpoints++)
                {
                    int at = endpoints * 8;
                    bytes[at] = (byte)(endpoints >> 8); bytes[at + 1] = (byte)endpoints;
                    ulong indices = (ulong)k * 0x249249249249UL;
                    for (int j = 0; j < 6; j++) bytes[at + j + 2] = (byte)(indices >> (8 * j));
                }
                Compare(bytes, 0, 256, 256, native);
            }
            var random = new Random(5817);
            foreach (int width in new[] { 1, 3, 63, 256 })
                foreach (int height in new[] { 1, 7, 193 })
                    foreach (int offset in new[] { 0, 128 })
                    {
                        var bytes = new byte[offset + width * height * 8]; random.NextBytes(bytes);
                        for (int at = offset; at < bytes.Length; at += 24) bytes[at] = 0;
                        Compare(bytes, offset, width, height, native);
                    }
            var bad = new byte[8]; var target = new int[4];
            Check(!CoverageNative.TryBuild(bad, -1, 1, 1, target), "Negative input offset rejected");
            Check(!CoverageNative.TryBuild(bad, 0, int.MaxValue, int.MaxValue, target), "Oversized map rejected");
            Check(!CoverageNative.TryBuild(bad, 0, 1, 1, new int[3]), "Short output rejected");
            var map = new byte[1024 * 1024 * 8]; random.NextBytes(map);
            for (int at = 0; at < map.Length; at += 8)
            {
                if ((at / 8) % 4 == 0) { map[at] = map[at + 1] = 0; }
                if ((at / 8) % 3 == 0) Array.Clear(map, at, 8);
            }
            var prefix = new int[1025 * 1025];
            OriginalPrefix(map, 1024, 1024, prefix);
            var oracle = new int[prefix.Length]; ReferencePrefix(map, 0, 1024, 1024, oracle);
            Check(prefix.SequenceEqual(oracle), "Previous implementation matches independent oracle");
            double reference = Time(() => OriginalPrefix(map, 1024, 1024, prefix));
            double managed = Time(() => CoverageZeroMap.BuildManaged(map, 0, 1024, 1024, prefix));
            double rust = native ? Time(() => { if (!CoverageNative.TryBuild(map, 0, 1024, 1024, prefix)) throw new Exception("Rust unavailable"); }) : 0;
            Console.WriteLine("Coverage prefix: " + checks + " checks passed; 1,048,576 blocks; reference=" + reference.ToString("F3")
                + " ms; managed=" + managed.ToString("F3") + " ms; Rust=" + rust.ToString("F3") + " ms");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
