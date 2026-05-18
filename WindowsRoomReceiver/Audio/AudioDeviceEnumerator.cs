using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace WindowsRoomReceiver.Audio;

public sealed record AudioOutputDevice(int DeviceId, string Name)
{
    public const int WaveMapper = -1;
    public override string ToString() => Name;
}

public static class AudioDeviceEnumerator
{
    public static IReadOnlyList<AudioOutputDevice> GetDevices()
    {
        var list = new List<AudioOutputDevice>
        {
            new AudioOutputDevice(AudioOutputDevice.WaveMapper, "System Default Output")
        };

        int count = waveOutGetNumDevs();
        for (int i = 0; i < count; ++i)
        {
            if (waveOutGetDevCaps((IntPtr)i, out WAVEOUTCAPS caps, Marshal.SizeOf<WAVEOUTCAPS>()) == 0)
            {
                string name = CleanName(caps.szPname);
                if (!string.IsNullOrWhiteSpace(name))
                {
                    list.Add(new AudioOutputDevice(i, name));
                }
            }
        }
        return list;
    }

    /// <summary>
    /// Strips trailing API tags such as "(WASAPI)", "(DirectSound)" and
    /// device-channel suffixes like " 1- " that some drivers attach.
    /// </summary>
    private static string CleanName(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return string.Empty;
        string name = raw.Trim();
        name = Regex.Replace(name, @"\s*\((?:WASAPI|DirectSound|MME|WDM|KS|ASIO)\)\s*$", string.Empty, RegexOptions.IgnoreCase);
        name = Regex.Replace(name, @"^\d+-\s*", string.Empty);
        return name.Trim();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WAVEOUTCAPS
    {
        public ushort wMid;
        public ushort wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szPname;
        public uint dwFormats;
        public ushort wChannels;
        public ushort wReserved1;
        public uint dwSupport;
    }

    [DllImport("winmm.dll")]
    private static extern int waveOutGetNumDevs();

    [DllImport("winmm.dll", CharSet = CharSet.Unicode, EntryPoint = "waveOutGetDevCapsW")]
    private static extern int waveOutGetDevCaps(IntPtr deviceID, out WAVEOUTCAPS caps, int cbSize);
}
