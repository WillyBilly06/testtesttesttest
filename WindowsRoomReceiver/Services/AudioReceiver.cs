using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using WindowsRoomReceiver.Audio;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

/// <summary>
/// Long-lived UDP audio receiver.
///
/// The room source streams audio to *both*:
///   1. Each authenticated client's per-device unicast endpoint
///      (works on locked-down enterprise/campus Wi-Fi where multicast
///      is blocked — the dependable path, scales to ~12 devices).
///   2. The room multicast group `239.10.10.10:5004` (works on
///      multicast-friendly networks — scales to hundreds of devices
///      with zero extra cost on the source).
///
/// We open both sockets here. Whichever arrives first wins; the
/// duplicate (if any) is dropped by sequence-number deduplication in
/// <see cref="ProcessPacket"/>. There is no application-level ACK or
/// retransmit on either path — true send-and-forget.
///
/// Authentication, key exchange, and heartbeats all stay on the
/// unicast control channel — completely separate from audio data.
/// </summary>
public sealed class AudioReceiver : IDisposable
{
    public const string DefaultMulticastGroup = "239.10.10.10";
    public const int DefaultMulticastPort = 5004;

    /// <summary>
    /// Capacity = 600 ms of stereo @ 48 kHz. This gives multicast bursts
    /// room to land without forcing steady playback latency above the
    /// target cushion.
    /// </summary>
    private const int RingCapacitySamples = 48_000 * 2 * 600 / 1000;
    private const int PcmSamplesPerPacket = 48_000 * 2 * 8 / 1000;
    private const int ReorderHoldPackets = 8;

    /// <summary>
    /// High-water pre-roll cushion. waveOut immediately pulls four
    /// 16 ms buffers (~64 ms) when playback starts, so priming at only
    /// 150 ms makes the visible ring drop below 100 ms right away.
    /// Prime higher so steady-state ring depth stays near 150 ms.
    /// </summary>
    private const int PrerollSamples = 48_000 * 2 * 240 / 1000;

    /// <summary>
    /// If multicast delivery falls below this low-watermark, pause into
    /// silence and rebuild the high-water cushion instead of running
    /// below the stable 150 ms target.
    /// </summary>
    private const int ReprimeSamples = 48_000 * 2 * 150 / 1000;

    private readonly UdpClient _unicast;
    private readonly UdpClient? _multicast;
    private readonly IPAddress _multicastGroup;
    private readonly SbcDecoder _decoder = new();
    private readonly PcmRingBuffer _ring = new(RingCapacitySamples);
    private readonly AudioPlaybackEngine _playback;
    private readonly object _sessionLock = new();
    private readonly object _seqLock = new();
    private readonly SortedDictionary<uint, PendingAudioPacket> _pendingPackets = new();
    private AudioSession? _session;
    private uint _nextSeq;
    private bool _haveNextSeq;
    private uint _multicastPackets;

    public int Port { get; }
    public IPAddress LocalAddress { get; }
    public bool MulticastJoined => _multicast != null;
    public uint PacketsReceived { get; private set; }
    public uint MulticastPacketsReceived => _multicastPackets;
    public uint SequenceGaps { get; private set; }
    public uint DecryptFailures { get; private set; }
    public uint DecodeErrors { get; private set; }
    public int BufferMs => (_ring.Available / 2) * 1000 / 48000;
    public DateTime LastPacketUtc { get; private set; } = DateTime.UtcNow;

    public AudioReceiver(IPAddress localAddress, int port = 47000, int outputDeviceId = AudioOutputDevice.WaveMapper)
    {
        LocalAddress = localAddress;
        _multicastGroup = IPAddress.Parse(DefaultMulticastGroup);
        _ring.PrerollSamples = PrerollSamples;
        _ring.ReprimeSamples = ReprimeSamples;
        _playback = new AudioPlaybackEngine(_ring, deviceId: outputDeviceId);

        // Unicast socket — bound on the chosen NIC at the requested
        // port. The source streams here for every client it has
        // accepted, so this path works even on networks that block
        // multicast.
        _unicast = UdpSocketUtil.CreateBoundUdp(port, allowPortFallback: true, localAddress: localAddress);
        try { _unicast.Client.ReceiveBufferSize = 1 << 20; } catch { /* best effort */ }
        Port = ((IPEndPoint)_unicast.Client.LocalEndPoint!).Port;

        // Multicast socket — joins the room group. Bound on Any:5004
        // with REUSEADDR so it can coexist with anything else on this
        // host listening on the same multicast port. JoinMulticastGroup
        // is given the chosen local interface so IGMP goes out on the
        // physical NIC (not on a virtual Hyper-V/WSL/VPN adapter).
        _multicast = TryOpenMulticastSocket(localAddress, _multicastGroup, DefaultMulticastPort);
    }

    private static UdpClient? TryOpenMulticastSocket(IPAddress localInterface, IPAddress group, int port)
    {
        UdpClient? udp = null;
        try
        {
            udp = new UdpClient(AddressFamily.InterNetwork);
            try { udp.Client.ExclusiveAddressUse = false; } catch { }
            try { udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true); } catch { }
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, port));
            try { udp.Client.ReceiveBufferSize = 1 << 20; } catch { }
            try
            {
                udp.JoinMulticastGroup(group, localInterface);
            }
            catch (SocketException)
            {
                udp.JoinMulticastGroup(group);
            }
            return udp;
        }
        catch
        {
            udp?.Dispose();
            return null;
        }
    }

    /// <summary>
    /// Sets / replaces the active session. Sequence tracking and the
    /// jitter buffer reset so the new session starts cleanly.
    /// </summary>
    public void SetSession(AudioSession session)
    {
        lock (_sessionLock)
        {
            _session = session;
            LastPacketUtc = DateTime.UtcNow;
        }
        lock (_seqLock)
        {
            _pendingPackets.Clear();
            _nextSeq = session.StartSequence;
            _haveNextSeq = false;
        }
        _ring.Reset();
    }

    public async Task RunAsync(CancellationToken ct)
    {
        _playback.Start();
        Task uni = Task.Run(() => ReceiveLoopAsync(_unicast, isMulticast: false, ct), ct);
        Task mc = _multicast != null
            ? Task.Run(() => ReceiveLoopAsync(_multicast, isMulticast: true, ct), ct)
            : Task.CompletedTask;
        await Task.WhenAll(uni, mc);
    }

    private async Task ReceiveLoopAsync(UdpClient udp, bool isMulticast, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await UdpSocketUtil.ReceiveAsyncNoCancelSocketOp(udp, ct);
            }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException) { return; }

            AudioSession? snapshot;
            lock (_sessionLock) snapshot = _session;
            if (snapshot == null) continue;

            try
            {
                ProcessPacket(result.Buffer, snapshot, isMulticast);
            }
            catch (CryptographicException)
            {
                DecryptFailures++;
            }
        }
    }

    private void ProcessPacket(byte[] packet, AudioSession session, bool fromMulticast)
    {
        if (packet.Length < 29) return;
        if (!AudioHeaderProtocolExtension.HasValidHeaderLikeAudio(packet)) return;
        uint streamId = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(8, 4));
        if (streamId != session.StreamId) return;
        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(12, 4));
        ushort frameBytes = BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(26, 2));
        if (frameBytes == 0 || packet.Length < 29 + frameBytes) return;

        byte[] encrypted = packet.AsSpan(29, frameBytes).ToArray();
        lock (_seqLock)
        {
            if (!_haveNextSeq)
            {
                _nextSeq = seq;
                _haveNextSeq = true;
            }

            // Old duplicate from RTN=3 after the decode cursor already
            // passed it. Safe to ignore.
            if (seq < _nextSeq) return;

            // Keep the first copy of each sequence. Later RTN copies are
            // useful only if the first one was lost.
            if (!_pendingPackets.ContainsKey(seq))
            {
                _pendingPackets.Add(seq, new PendingAudioPacket(encrypted, fromMulticast));
                PacketsReceived++;
                if (fromMulticast) _multicastPackets++;
                LastPacketUtc = DateTime.UtcNow;
            }

            DrainPendingPacketsLocked(session);
        }
    }

    private static byte[] MakeAudioNonce(uint streamId, uint seq)
    {
        byte[] nonce = new byte[16];
        "SBCA"u8.CopyTo(nonce);
        BinaryPrimitives.WriteUInt32LittleEndian(nonce.AsSpan(4, 4), streamId);
        BinaryPrimitives.WriteUInt32LittleEndian(nonce.AsSpan(8, 4), seq);
        return nonce;
    }

    private void DrainPendingPacketsLocked(AudioSession session)
    {
        if (!_decoder.IsAvailable) return;

        while (_pendingPackets.Count > 0)
        {
            if (_pendingPackets.Remove(_nextSeq, out PendingAudioPacket? pending))
            {
                try
                {
                    byte[] nonce = MakeAudioNonce(session.StreamId, _nextSeq);
                    byte[] decrypted = CryptoUtil.AesCtr(session.Key, nonce, pending.EncryptedPayload);
                    short[] pcm = _decoder.Decode(decrypted);
                    if (pcm.Length > 0) _ring.Write(pcm, 0, pcm.Length);
                }
                catch (Exception)
                {
                    DecodeErrors++;
                    _ring.Write(new short[PcmSamplesPerPacket], 0, PcmSamplesPerPacket);
                }
                _nextSeq++;
                continue;
            }

            // Missing _nextSeq. Hold a small number of future packets so
            // RTN duplicates can arrive and fill the gap. If the future
            // queue gets too deep, conceal exactly one missing 8 ms audio
            // packet with silence and continue in order.
            if (_pendingPackets.Count < ReorderHoldPackets)
            {
                return;
            }

            SequenceGaps++;
            _ring.Write(new short[PcmSamplesPerPacket], 0, PcmSamplesPerPacket);
            _nextSeq++;
        }
    }

    public void Dispose()
    {
        if (_multicast != null)
        {
            try { _multicast.DropMulticastGroup(_multicastGroup); } catch { }
            _multicast.Dispose();
        }
        _playback.Dispose();
        _unicast.Dispose();
    }
}

internal sealed record PendingAudioPacket(byte[] EncryptedPayload, bool FromMulticast);

internal static class AudioHeaderProtocolExtension
{
    public static bool HasValidHeaderLikeAudio(byte[] data)
    {
        return data.Length >= 29 &&
               BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(0, 4)) == RoomProtocol.Magic &&
               data[4] == RoomProtocol.Version &&
               data[5] == 4;
    }
}
