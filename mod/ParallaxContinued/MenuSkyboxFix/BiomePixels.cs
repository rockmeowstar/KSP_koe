using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;

namespace MenuSkyboxFix
{
    public sealed class BiomeExpansion
    {
        public byte[] Bytes;
        public int Workers;
    }

    public static class BiomePixels
    {
        public static BiomeExpansion Expand(byte[] indices, byte[] palette, int width, int height, int degree)
        {
            if (indices == null
                || palette == null
                || width < 1
                || height < 1
                || degree < 1
                || palette.Length == 0
                || palette.Length % 3 != 0
                || palette.Length > 768
                || indices.LongLength != (long)width * height)
                throw new ArgumentException("Invalid biome input");
            var output = new byte[checked(indices.Length * 3)];
            int parts = Math.Min(degree, height);
            var threads = new ConcurrentDictionary<int, byte>();
            Parallel.For(0, parts, new ParallelOptions { MaxDegreeOfParallelism = degree }, part =>
            {
                threads.TryAdd(Thread.CurrentThread.ManagedThreadId, 0);
                int first = (int)((long)height * part / parts) * width, last = (int)((long)height * (part + 1) / parts) * width;
                for (int i = first; i < last; i++)
                {
                    int p = indices[i] * 3, j = i * 3;
                    output[j] = palette[p];
                    output[j + 1] = palette[p + 1];
                    output[j + 2] = palette[p + 2];
                }
            });
            return new BiomeExpansion
            {
                Bytes = output,
                Workers = threads.Count
            };
        }
    }
}
