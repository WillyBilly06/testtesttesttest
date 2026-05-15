using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using WindowsRoomReceiver.Audio;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public sealed class AudioReceiver : IDisposable
{
    private readonly UdpClient _udp;
    private readonly SbcDecoder _decoder = new();
    private readonly AudioPlaybackEngine _playback = new();
    private uint _lastSeq;

    public int Port { get; }
    public IPAddress LocalAddress { get; }
    public ManualUdpEndpoint? ManualEndpoint { get; }
    public uint PacketsReceived { get; private set; }
    public uint SequenceGaps { get; private set; }
    public uint DecryptFailures { get; private set; }
    public uint DecodeErrors { get; private set; }
    public int BufferMs => _playback.BufferMs;

    public AudioReceiver(IPAddress localAddress, int port = 47000, ManualUdpEndpoint? manualEndpoint = null)
    {
        LocalAddress = localAddress;
        ManualEndpoint = manualEndpoint;

        if (manualEndpoint is { IsMulticast: true })
        {
            _udp = UdpSocketUtil.CreateMulticastReceiver(manualEndpoint.Address, manualEndpoint.Port, localAddress);
            Port = manualEndpoint.Port;
        }
        else
        {
            // For normal unicast audio, bind to the selected local adapter. If the user
            // entered a non-multicast IP, it is treated as a display/intent hint; the
            // actual receive address remains the adapter that can reach the room.
            int bindPort = manualEndpoint?.Port ?? port;
            _udp = UdpSocketUtil.CreateBoundUdp(bindPort, allowPortFallback: manualEndpoint == null, localAddress: localAddress);
            Port = ((System.Net.IPEndPoint)_udp.Client.LocalEndPoint!).Port;
        }
    }

    public async Task RunAsync(AudioSession session, CancellationToken ct)
    {
        _playback.Start();
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result = await UdpSocketUtil.ReceiveAsyncNoCancelSocketOp(_udp, ct);
            try
            {
                ProcessPacket(result.Buffer, session);
            }
            catch (CryptographicException)
            {
                DecryptFailures++;
            }
        }
    }

    private void ProcessPacket(byte[] packet, AudioSession session)
    {
        if (packet.Length < 29) return;
        if (!AudioHeaderProtocolExtension.HasValidHeaderLikeAudio(packet)) return;
        uint streamId = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(8, 4));
        if (streamId != session.StreamId) return;
        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(12, 4));
        ushort frameBytes = BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(26, 2));
        if (frameBytes == 0 || packet.Length < 29 + frameBytes) return;
        if (_lastSeq != 0 && seq > _lastSeq + 1) SequenceGaps += seq - _lastSeq - 1;
        if (_lastSeq != 0 && seq <= _lastSeq) return;
        _lastSeq = seq;
        byte[] nonce = MakeAudioNonce(streamId, seq);
        byte[] decrypted = CryptoUtil.AesCtr(session.Key, nonce, packet.AsSpan(29, frameBytes));
        PacketsReceived++;
        if (!_decoder.IsAvailable) return;
        try
        {
            _playback.PushPcm(_decoder.Decode(decrypted));
        }
        catch (Exception)
        {
            DecodeErrors++;
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

    public void Dispose()
    {
        _playback.Dispose();
        _udp.Dispose();
    }
}

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
