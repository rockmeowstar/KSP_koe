using System;
using System.Runtime.InteropServices;
static class EncoderPresenceTests
{
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true)]static extern IntPtr GetProcAddress(IntPtr module,string name);
    [DllImport("kernel32.dll")]static extern bool FreeLibrary(IntPtr module);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]delegate int Probe();
    static int Main(string[] args)
    {
        IntPtr module=IntPtr.Zero;
        try{
            module=LoadLibraryW(args[0]);if(module==IntPtr.Zero)throw new Exception("Encoder library failed to load");
            var address=GetProcAddress(module,"ProbeEncoder");if(address==IntPtr.Zero)throw new Exception("Availability probe export missing");
            var probe=(Probe)Marshal.GetDelegateForFunctionPointer(address,typeof(Probe));
            if(probe()!=0)throw new Exception("Missing Unity module must report unavailable");
            Console.WriteLine("ENCODER_ABSENCE_PASS: real native DLL, no Unity loaded, no hook initialized");return 0;
        }catch(Exception error){Console.Error.WriteLine(error);return 1;}
        finally{if(module!=IntPtr.Zero)FreeLibrary(module);}
    }
}
