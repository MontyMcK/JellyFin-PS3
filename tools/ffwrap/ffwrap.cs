// ffwrap -- stands in for Jellyfin's ffmpeg.exe and caps how fast ffmpeg pulls
// an INTERNET source (debrid / Gelato streams).
//
// Why: with the server PC wired to the router, a transcode reads its remote
// input as fast as the internet allows (~110 Mbps here, ffmpeg at 4x).  That
// saturates the router, and while it is saturated the router stops passing
// LAN traffic between the PC and the PS3 -- pings to the console were lost or
// took 0.3-1.3 s, and the PS3 stream fell to ~0.5 Mbps (measured 2026-10-13).
// Every PS3 stream that ran clean that night was one the server served with no
// ffmpeg pulling.
//
// What it does: for an http(s) input it inserts
//     -readrate 1.25 -readrate_initial_burst 20
// before that -i, so ffmpeg reads at 1.25x the media's own rate after a 20 s
// head start.  Everything else is passed through untouched: the command line
// is forwarded verbatim (no re-quoting), the std handles are inherited, the
// exit code is returned, and ffmpeg runs in a job object so it dies with this
// process when Jellyfin kills it.
//
// The real ffmpeg is the one next to Jellyfin: C:\Program Files\Jellyfin\Server\ffmpeg.exe
// (override with FFWRAP_REAL).  ffwrap.log next to this exe records each rewrite.

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

static class FfWrap
{
    const string Inject = "-readrate 1.25 -readrate_initial_burst 20 ";

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr a, string name);
    [DllImport("kernel32.dll")]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtLimit info, int len);
    [DllImport("kernel32.dll")]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);

    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimit { public long a, b; public uint flags; public UIntPtr c, d; public uint e; public UIntPtr f; public uint g, h; }
    [StructLayout(LayoutKind.Sequential)]
    struct IoCounters { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtLimit { public BasicLimit basic; public IoCounters io; public UIntPtr p, j, pp, jp; }

    static string ArgsOf(string cmd)
    {
        // Strip argv[0] exactly as Windows wrote it, keep the rest byte for byte.
        int i = 0;
        if (cmd.Length > 0 && cmd[0] == '"') { i = cmd.IndexOf('"', 1); i = i < 0 ? cmd.Length : i + 1; }
        else { while (i < cmd.Length && cmd[i] != ' ' && cmd[i] != '\t') i++; }
        return cmd.Substring(i).TrimStart();
    }

    static string Rewrite(string args)
    {
        if (args.Contains("-readrate") || args.Contains(" -re ")) return args;
        foreach (var pat in new[] { "-i \"http", "-i http" })
        {
            int k = args.IndexOf(pat, StringComparison.OrdinalIgnoreCase);
            if (k >= 0 && (k == 0 || args[k - 1] == ' ')) return args.Substring(0, k) + Inject + args.Substring(k);
        }
        return args;
    }

    static int Main()
    {
        string real = Environment.GetEnvironmentVariable("FFWRAP_REAL");
        if (string.IsNullOrEmpty(real)) real = @"C:\Program Files\Jellyfin\Server\ffmpeg.exe";
        string args = ArgsOf(Environment.CommandLine);
        string outArgs = Rewrite(args);
        if (!ReferenceEquals(outArgs, args))
        {
            try
            {
                string log = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ffwrap.log");
                File.AppendAllText(log, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " readrate injected\r\n");
            }
            catch { }
        }

        var psi = new ProcessStartInfo(real, outArgs) { UseShellExecute = false };
        Process p;
        try { p = Process.Start(psi); }
        catch (Exception e) { Console.Error.WriteLine("ffwrap: cannot start " + real + ": " + e.Message); return 1; }

        try
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            var info = new ExtLimit();
            info.basic.flags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtLimit)));
            AssignProcessToJobObject(job, p.Handle);
            // The handle is deliberately never closed: it closes when this process
            // ends, and then the job kills ffmpeg with it.
        }
        catch { }

        p.WaitForExit();
        return p.ExitCode;
    }
}
