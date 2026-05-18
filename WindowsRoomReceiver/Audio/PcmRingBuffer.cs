namespace WindowsRoomReceiver.Audio;

/// <summary>
/// Thread-safe circular buffer of 16-bit PCM samples (interleaved).
/// Producer (network/decode thread) writes; consumer (playback worker)
/// reads in fixed-size chunks. On overflow the oldest samples are
/// dropped — stale audio is never useful. On underrun the reader gets
/// silence so the wave output never starves.
///
/// Includes a pre-roll gate: <see cref="Read"/> returns silence (and
/// reports zero "real" samples) until the buffer has accumulated
/// <see cref="PrerollSamples"/>. This builds a deliberate jitter
/// cushion at startup and after every underrun, so the steady-state
/// fill stays around the pre-roll target instead of draining to zero.
/// </summary>
public sealed class PcmRingBuffer
{
    private readonly short[] _buffer;
    private readonly object _lock = new();
    private int _writeIdx;
    private int _readIdx;
    private int _count;
    private long _totalDropped;
    private long _totalWritten;
    private long _totalRead;
    private long _totalUnderrunSamples;
    private bool _primed;

    public int Capacity => _buffer.Length;
    public int PrerollSamples { get; set; }
    public int ReprimeSamples { get; set; }
    public int Available { get { lock (_lock) return _count; } }
    public bool Primed { get { lock (_lock) return _primed; } }
    public long TotalDropped => Interlocked.Read(ref _totalDropped);
    public long TotalUnderrunSamples => Interlocked.Read(ref _totalUnderrunSamples);

    public PcmRingBuffer(int capacitySamples)
    {
        if (capacitySamples <= 0) throw new ArgumentOutOfRangeException(nameof(capacitySamples));
        _buffer = new short[capacitySamples];
    }

    public void Reset()
    {
        lock (_lock)
        {
            _readIdx = 0;
            _writeIdx = 0;
            _count = 0;
            _primed = false;
        }
    }

    public void Write(short[] samples, int offset, int length)
    {
        if (length <= 0) return;
        lock (_lock)
        {
            // If the incoming batch is bigger than the whole ring, only
            // keep the tail (newest audio).
            if (length >= _buffer.Length)
            {
                _totalDropped += _count + (length - _buffer.Length);
                offset += length - _buffer.Length;
                length = _buffer.Length;
                _readIdx = 0;
                _writeIdx = 0;
                _count = 0;
            }

            // If writing would overflow, advance the read pointer (drop
            // oldest) so the new audio fits at the head of the ring.
            int free = _buffer.Length - _count;
            if (length > free)
            {
                int dropCount = length - free;
                _readIdx = (_readIdx + dropCount) % _buffer.Length;
                _count -= dropCount;
                _totalDropped += dropCount;
            }

            int firstChunk = Math.Min(length, _buffer.Length - _writeIdx);
            Array.Copy(samples, offset, _buffer, _writeIdx, firstChunk);
            int remainder = length - firstChunk;
            if (remainder > 0)
            {
                Array.Copy(samples, offset + firstChunk, _buffer, 0, remainder);
            }
            _writeIdx = (_writeIdx + length) % _buffer.Length;
            _count += length;
            _totalWritten += length;

            // Promote to "primed" once we cross the pre-roll threshold.
            // Until then the consumer side (Read) returns silence.
            if (!_primed && _count >= PrerollSamples)
            {
                _primed = true;
            }
        }
    }

    /// <summary>
    /// Reads up to <paramref name="length"/> samples into <paramref name="dest"/>.
    /// Always writes exactly <paramref name="length"/> samples: anything
    /// not satisfied from the ring is padded with silence (0) so the
    /// caller can hand a fixed-size buffer straight to waveOutWrite.
    /// Returns the number of "real" samples (non-silence).
    /// </summary>
    public int Read(short[] dest, int offset, int length)
    {
        if (length <= 0) return 0;
        lock (_lock)
        {
            // Pre-roll gate: while we haven't crossed the priming
            // threshold, hand back silence so the wave output stays
            // alive but the ring keeps filling. As soon as Write()
            // pushes us past PrerollSamples, _primed flips to true and
            // we start delivering real audio with that cushion intact.
            if (!_primed)
            {
                Array.Clear(dest, offset, length);
                _totalUnderrunSamples += length;
                return 0;
            }

            int realSamples = Math.Min(length, _count);
            int firstChunk = Math.Min(realSamples, _buffer.Length - _readIdx);
            Array.Copy(_buffer, _readIdx, dest, offset, firstChunk);
            int remainder = realSamples - firstChunk;
            if (remainder > 0)
            {
                Array.Copy(_buffer, 0, dest, offset + firstChunk, remainder);
            }
            _readIdx = (_readIdx + realSamples) % _buffer.Length;
            _count -= realSamples;
            _totalRead += realSamples;

            int silence = length - realSamples;
            if (silence > 0)
            {
                Array.Clear(dest, offset + realSamples, silence);
                _totalUnderrunSamples += silence;
                // Underran -> drop priming so the next refill rebuilds
                // the cushion before audio resumes (avoids the dreaded
                // perpetually-empty ring).
                _primed = false;
            }
            else if (ReprimeSamples > 0 && _count < ReprimeSamples)
            {
                // Multicast over infrastructure Wi-Fi often arrives in
                // DTIM-sized bursts. If the buffer falls below the low
                // watermark, stop consuming real audio and rebuild the
                // cushion instead of running near-empty and producing a
                // stream of tiny underruns.
                _primed = false;
            }
            return realSamples;
        }
    }
}
