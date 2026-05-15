using System.Runtime.InteropServices;

namespace WindowsRoomReceiver.Audio;

public sealed class AudioPlaybackEngine : IDisposable
{
    private const int WAVE_FORMAT_PCM = 1;
    private const int CALLBACK_NULL = 0;
    private readonly IntPtr _waveOut;
    private readonly List<GCHandle> _pinnedBuffers = new();
    private readonly object _lock = new();
    private readonly int _avgBytesPerSecond;
    private int _queuedBytes;
    private bool _started;

    public AudioPlaybackEngine(int sampleRate = 48000, int channels = 2)
    {
        _avgBytesPerSecond = sampleRate * channels * sizeof(short);
        WAVEFORMATEX format = new()
        {
            wFormatTag = WAVE_FORMAT_PCM,
            nChannels = (ushort)channels,
            nSamplesPerSec = (uint)sampleRate,
            wBitsPerSample = 16,
            nBlockAlign = (ushort)(channels * sizeof(short)),
            nAvgBytesPerSec = (uint)_avgBytesPerSecond
        };
        int result = waveOutOpen(out _waveOut, UIntPtr.Zero, ref format, IntPtr.Zero, IntPtr.Zero, CALLBACK_NULL);
        if (result != 0) throw new InvalidOperationException($"No audio output device is available. waveOutOpen={result}");
    }

    public int BufferMs => _avgBytesPerSecond == 0 ? 0 : (int)((long)_queuedBytes * 1000 / _avgBytesPerSecond);

    public void Start() => _started = true;

    public void Stop()
    {
        lock (_lock)
        {
            _started = false;
            waveOutReset(_waveOut);
            foreach (GCHandle handle in _pinnedBuffers)
            {
                if (handle.IsAllocated) handle.Free();
            }
            _pinnedBuffers.Clear();
            _queuedBytes = 0;
        }
    }

    public void PushPcm(short[] samples)
    {
        if (!_started || samples.Length == 0) return;
        byte[] bytes = new byte[samples.Length * sizeof(short)];
        Buffer.BlockCopy(samples, 0, bytes, 0, bytes.Length);
        GCHandle dataHandle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        WAVEHDR header = new()
        {
            lpData = dataHandle.AddrOfPinnedObject(),
            dwBufferLength = (uint)bytes.Length
        };
        GCHandle headerHandle = GCHandle.Alloc(header, GCHandleType.Pinned);
        lock (_lock)
        {
            _pinnedBuffers.Add(dataHandle);
            _pinnedBuffers.Add(headerHandle);
            _queuedBytes += bytes.Length;
        }
        IntPtr hdrPtr = headerHandle.AddrOfPinnedObject();
        int headerSize = Marshal.SizeOf<WAVEHDR>();
        if (waveOutPrepareHeader(_waveOut, hdrPtr, headerSize) == 0)
        {
            waveOutWrite(_waveOut, hdrPtr, headerSize);
        }
        CleanupCompletedBuffers();
    }

    public void Dispose()
    {
        Stop();
        waveOutClose(_waveOut);
    }

    private void CleanupCompletedBuffers()
    {
        lock (_lock)
        {
            for (int i = _pinnedBuffers.Count - 1; i >= 1; i -= 2)
            {
                GCHandle headerHandle = _pinnedBuffers[i];
                if (!headerHandle.IsAllocated) continue;
                WAVEHDR header = Marshal.PtrToStructure<WAVEHDR>(headerHandle.AddrOfPinnedObject());
                if ((header.dwFlags & WaveHdrDone) == 0) continue;
                _queuedBytes = Math.Max(0, _queuedBytes - (int)header.dwBufferLength);
                waveOutUnprepareHeader(_waveOut, headerHandle.AddrOfPinnedObject(), Marshal.SizeOf<WAVEHDR>());
                headerHandle.Free();
                GCHandle dataHandle = _pinnedBuffers[i - 1];
                if (dataHandle.IsAllocated) dataHandle.Free();
                _pinnedBuffers.RemoveAt(i);
                _pinnedBuffers.RemoveAt(i - 1);
            }
        }
    }

    private const uint WaveHdrDone = 0x00000001;

    [StructLayout(LayoutKind.Sequential)]
    private struct WAVEFORMATEX
    {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WAVEHDR
    {
        public IntPtr lpData;
        public uint dwBufferLength;
        public uint dwBytesRecorded;
        public nuint dwUser;
        public uint dwFlags;
        public uint dwLoops;
        public IntPtr lpNext;
        public nuint reserved;
    }

    [DllImport("winmm.dll")]
    private static extern int waveOutOpen(out IntPtr hWaveOut, UIntPtr uDeviceID, ref WAVEFORMATEX lpFormat,
        IntPtr dwCallback, IntPtr dwInstance, int dwFlags);

    [DllImport("winmm.dll")]
    private static extern int waveOutPrepareHeader(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);

    [DllImport("winmm.dll")]
    private static extern int waveOutWrite(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);

    [DllImport("winmm.dll")]
    private static extern int waveOutUnprepareHeader(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);

    [DllImport("winmm.dll")]
    private static extern int waveOutReset(IntPtr hWaveOut);

    [DllImport("winmm.dll")]
    private static extern int waveOutClose(IntPtr hWaveOut);
}
