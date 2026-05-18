using System.Runtime.InteropServices;

namespace WindowsRoomReceiver.Audio;

/// <summary>
/// Low-latency PCM playback with a small fixed pool of <c>waveOut</c>
/// buffers and a dedicated worker thread that keeps them full.
///
/// Design goals (driven by real-world failure modes we hit before):
///   - Constant inflight depth (~64 ms). The hardware queue never
///     drains and never balloons, so latency stays steady regardless
///     of Wi-Fi burstiness.
///   - PCM jitter buffer is upstream (<see cref="PcmRingBuffer"/>).
///     This engine just pulls fixed-size chunks from the ring; if the
///     ring is short, the missing tail of a chunk is silence — no
///     click, no underrun stutter.
///   - Survives device hot-removal: bounded memory, automatic recovery
///     when the device comes back.
///
/// Sample format: signed 16-bit interleaved L/R at 48 kHz.
/// </summary>
public sealed class AudioPlaybackEngine : IDisposable
{
    private const int WAVE_FORMAT_PCM = 1;
    private const int CALLBACK_NULL = 0;

    // Tuning: 4 slots × 16 ms = ~64 ms in-flight, ~16 ms granularity.
    private const int BufferSlots = 4;
    private const int BufferMs = 16;

    private readonly object _lock = new();
    private readonly int _sampleRate;
    private readonly int _channels;
    private readonly int _deviceId;
    private readonly int _avgBytesPerSecond;
    private readonly int _samplesPerBuffer;
    private readonly PcmRingBuffer _source;

    private IntPtr _waveOut;
    private GCHandle[] _dataHandles = Array.Empty<GCHandle>();
    private GCHandle[] _headerHandles = Array.Empty<GCHandle>();

    private CancellationTokenSource? _workerCts;
    private Thread? _worker;
    private bool _started;
    private bool _disposed;
    private bool _deviceSick;
    private DateTime _lastDeviceReopenAttempt = DateTime.MinValue;

    public bool DeviceSick { get { lock (_lock) return _deviceSick; } }

    public AudioPlaybackEngine(PcmRingBuffer source, int sampleRate = 48000, int channels = 2, int deviceId = AudioOutputDevice.WaveMapper)
    {
        _source = source;
        _sampleRate = sampleRate;
        _channels = channels;
        _deviceId = deviceId;
        _avgBytesPerSecond = sampleRate * channels * sizeof(short);
        // 16 ms × 48 000 Hz × 2 ch = 1536 samples.
        _samplesPerBuffer = (sampleRate * channels * BufferMs) / 1000;
        OpenDevice();
        AllocateBuffers();
    }

    public void Start()
    {
        lock (_lock)
        {
            if (_started || _disposed) return;
            _started = true;
            _workerCts = new CancellationTokenSource();
            _worker = new Thread(WorkerLoop)
            {
                Name = "wpf-wave-out",
                IsBackground = true,
                Priority = ThreadPriority.AboveNormal
            };
            _worker.Start(_workerCts.Token);
        }
    }

    public void Stop()
    {
        Thread? worker;
        CancellationTokenSource? cts;
        lock (_lock)
        {
            cts = _workerCts;
            worker = _worker;
            _started = false;
            _workerCts = null;
            _worker = null;
        }
        cts?.Cancel();
        worker?.Join(500);

        lock (_lock)
        {
            ResetWaveOutLocked();
        }
    }

    public void Dispose()
    {
        Stop();
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            FreeBuffers();
            if (_waveOut != IntPtr.Zero)
            {
                waveOutClose(_waveOut);
                _waveOut = IntPtr.Zero;
            }
        }
    }

    // ====================================================================

    private void OpenDevice()
    {
        WAVEFORMATEX format = new()
        {
            wFormatTag = WAVE_FORMAT_PCM,
            nChannels = (ushort)_channels,
            nSamplesPerSec = (uint)_sampleRate,
            wBitsPerSample = 16,
            nBlockAlign = (ushort)(_channels * sizeof(short)),
            nAvgBytesPerSec = (uint)_avgBytesPerSecond
        };
        UIntPtr device = _deviceId == AudioOutputDevice.WaveMapper
            ? unchecked((UIntPtr)(ulong)(long)-1)
            : (UIntPtr)(uint)_deviceId;
        int result = waveOutOpen(out _waveOut, device, ref format, IntPtr.Zero, IntPtr.Zero, CALLBACK_NULL);
        if (result != 0)
            throw new InvalidOperationException($"Could not open audio output device (waveOutOpen={result}).");
    }

    private void AllocateBuffers()
    {
        _dataHandles = new GCHandle[BufferSlots];
        _headerHandles = new GCHandle[BufferSlots];
        for (int i = 0; i < BufferSlots; ++i)
        {
            byte[] bytes = new byte[_samplesPerBuffer * sizeof(short)];
            _dataHandles[i] = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            WAVEHDR header = new()
            {
                lpData = _dataHandles[i].AddrOfPinnedObject(),
                dwBufferLength = (uint)bytes.Length,
                dwFlags = WaveHdrDone   // start "done" so the worker submits immediately
            };
            _headerHandles[i] = GCHandle.Alloc(header, GCHandleType.Pinned);
        }
    }

    private void FreeBuffers()
    {
        int headerSize = Marshal.SizeOf<WAVEHDR>();
        for (int i = 0; i < _headerHandles.Length; ++i)
        {
            if (_headerHandles[i].IsAllocated)
            {
                try { waveOutUnprepareHeader(_waveOut, _headerHandles[i].AddrOfPinnedObject(), headerSize); } catch { }
                _headerHandles[i].Free();
            }
        }
        for (int i = 0; i < _dataHandles.Length; ++i)
        {
            if (_dataHandles[i].IsAllocated) _dataHandles[i].Free();
        }
        _headerHandles = Array.Empty<GCHandle>();
        _dataHandles = Array.Empty<GCHandle>();
    }

    private void WorkerLoop(object? state)
    {
        CancellationToken ct = (CancellationToken)state!;
        short[] tempPcm = new short[_samplesPerBuffer];
        int headerSize = Marshal.SizeOf<WAVEHDR>();
        const int sleepMsWhenAllBusy = 2;

        while (!ct.IsCancellationRequested)
        {
            bool didWork = false;

            lock (_lock)
            {
                if (!_started)
                {
                    return;
                }

                // If the device is sick, periodically try to reopen.
                if (_deviceSick)
                {
                    TryRecoverDeviceLocked();
                    if (_deviceSick)
                    {
                        // Drain the source so backlog doesn't grow while
                        // the device is missing — read and discard.
                        while (_source.Available >= _samplesPerBuffer)
                        {
                            _source.Read(tempPcm, 0, _samplesPerBuffer);
                        }
                    }
                }
                else
                {
                    for (int i = 0; i < _headerHandles.Length; ++i)
                    {
                        if (!_headerHandles[i].IsAllocated) continue;
                        IntPtr hdrPtr = _headerHandles[i].AddrOfPinnedObject();
                        WAVEHDR header = Marshal.PtrToStructure<WAVEHDR>(hdrPtr);
                        if ((header.dwFlags & WaveHdrDone) == 0) continue;

                        // Recycle this slot: unprepare → fill → prepare → write.
                        if ((header.dwFlags & WaveHdrPrepared) != 0)
                        {
                            waveOutUnprepareHeader(_waveOut, hdrPtr, headerSize);
                        }

                        _source.Read(tempPcm, 0, _samplesPerBuffer);
                        byte[] data = GCHandleBytes(_dataHandles[i]);
                        Buffer.BlockCopy(tempPcm, 0, data, 0, _samplesPerBuffer * sizeof(short));

                        // Refresh the pinned header
                        WAVEHDR newHeader = new()
                        {
                            lpData = _dataHandles[i].AddrOfPinnedObject(),
                            dwBufferLength = (uint)(_samplesPerBuffer * sizeof(short))
                        };
                        Marshal.StructureToPtr(newHeader, hdrPtr, fDeleteOld: false);

                        int prep = waveOutPrepareHeader(_waveOut, hdrPtr, headerSize);
                        int wr = (prep == 0) ? waveOutWrite(_waveOut, hdrPtr, headerSize) : prep;
                        if (prep != 0 || wr != 0)
                        {
                            MarkDeviceSickLocked();
                            break;
                        }
                        didWork = true;
                    }
                }
            }

            if (!didWork)
            {
                try { Task.Delay(sleepMsWhenAllBusy, ct).Wait(ct); }
                catch (OperationCanceledException) { return; }
            }
        }
    }

    private void MarkDeviceSickLocked()
    {
        if (_deviceSick) return;
        _deviceSick = true;
        try { if (_waveOut != IntPtr.Zero) waveOutReset(_waveOut); } catch { }
    }

    private void TryRecoverDeviceLocked()
    {
        if (!_deviceSick || _disposed) return;
        DateTime now = DateTime.UtcNow;
        if ((now - _lastDeviceReopenAttempt).TotalMilliseconds < 1500) return;
        _lastDeviceReopenAttempt = now;

        try
        {
            if (_waveOut != IntPtr.Zero)
            {
                try { waveOutReset(_waveOut); } catch { }
                try { waveOutClose(_waveOut); } catch { }
                _waveOut = IntPtr.Zero;
            }
            // Re-prepare all buffer headers since old _waveOut handle is gone.
            int headerSize = Marshal.SizeOf<WAVEHDR>();
            for (int i = 0; i < _headerHandles.Length; ++i)
            {
                if (!_headerHandles[i].IsAllocated) continue;
                WAVEHDR reset = new()
                {
                    lpData = _dataHandles[i].AddrOfPinnedObject(),
                    dwBufferLength = (uint)(_samplesPerBuffer * sizeof(short)),
                    dwFlags = WaveHdrDone
                };
                Marshal.StructureToPtr(reset, _headerHandles[i].AddrOfPinnedObject(), fDeleteOld: false);
            }

            OpenDevice();
            _deviceSick = false;
        }
        catch
        {
            // Stay sick; we'll try again on the next worker tick.
        }
    }

    private void ResetWaveOutLocked()
    {
        if (_waveOut == IntPtr.Zero) return;
        try { waveOutReset(_waveOut); } catch { }
        int headerSize = Marshal.SizeOf<WAVEHDR>();
        for (int i = 0; i < _headerHandles.Length; ++i)
        {
            if (!_headerHandles[i].IsAllocated) continue;
            WAVEHDR h = Marshal.PtrToStructure<WAVEHDR>(_headerHandles[i].AddrOfPinnedObject());
            if ((h.dwFlags & WaveHdrPrepared) != 0)
            {
                try { waveOutUnprepareHeader(_waveOut, _headerHandles[i].AddrOfPinnedObject(), headerSize); } catch { }
            }
        }
    }

    private static byte[] GCHandleBytes(GCHandle h) => (byte[])h.Target!;

    private const uint WaveHdrDone = 0x00000001;
    private const uint WaveHdrPrepared = 0x00000002;

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

    [DllImport("winmm.dll")] private static extern int waveOutOpen(out IntPtr hWaveOut, UIntPtr uDeviceID, ref WAVEFORMATEX lpFormat, IntPtr dwCallback, IntPtr dwInstance, int dwFlags);
    [DllImport("winmm.dll")] private static extern int waveOutPrepareHeader(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] private static extern int waveOutWrite(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] private static extern int waveOutUnprepareHeader(IntPtr hWaveOut, IntPtr lpWaveOutHdr, int uSize);
    [DllImport("winmm.dll")] private static extern int waveOutReset(IntPtr hWaveOut);
    [DllImport("winmm.dll")] private static extern int waveOutClose(IntPtr hWaveOut);
}
