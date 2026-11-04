using System;
using System.Runtime.InteropServices;

namespace KerbalOverEngineering.Options
{
    public sealed class LoadingResources
    {
        public readonly int Workers, DecodeWindow;
        public readonly long DdsBudget, EveBudget, PngBudget, BiomeBudget, NoiseBudget;
        static LoadingResources current;
        public static LoadingResources Current { get { return current ?? (current = Detect()); } }
        public LoadingResources(int processors, long totalMemory, long availableMemory)
        {
            processors = Math.Max(1, processors);
            // Share a bounded fraction of currently available physical memory.
            // Budgets are aggregate reservations, not individual array lengths.
            long pool = totalMemory > 0 && availableMemory > 0 ? Math.Min(totalMemory / 16, Math.Min(totalMemory, availableMemory) / 8) : 0;
            DdsBudget = EveBudget = BiomeBudget = NoiseBudget = pool / 8;
            PngBudget = pool / 2;
            Workers = pool == 0 ? 1 : (int)Math.Max(1, Math.Min(processors, pool / (1024 * 1024)));
            DecodeWindow = Workers;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus
        {
            public uint Length, Load;
            public ulong TotalPhysical, AvailablePhysical, TotalPage, AvailablePage, TotalVirtual, AvailableVirtual, AvailableExtended;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
        static LoadingResources Detect()
        {
            long total = 0, available = 0;
            if (Environment.OSVersion.Platform == PlatformID.Win32NT)
            {
                try
                {
                    var status = new MemoryStatus { Length = (uint)Marshal.SizeOf(typeof(MemoryStatus)) };
                    if (GlobalMemoryStatusEx(ref status)) { total = checked((long)status.TotalPhysical); available = checked((long)status.AvailablePhysical); }
                }
                catch { total = available = 0; }
            }
            // Unknown memory availability disables speculative queues; normal
            // synchronous loading remains available on other platforms.
            return new LoadingResources(Environment.ProcessorCount, total, available);
        }
    }
}
