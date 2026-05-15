using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public sealed record AudioSession(byte[] Key, uint StreamId, uint SessionId, uint StartSequence, int FrameBytes, int Channels);

public sealed class AuthClient : IDisposable
{
    private readonly SecureCredentialStore _store;
    private readonly byte[] _clientId;
    private UdpClient? _control;
    private CancellationTokenSource? _heartbeatCts;

    public AuthClient(SecureCredentialStore store)
    {
        _store = store;
        _clientId = store.GetOrCreateClientId();
    }

    public async Task<AudioSession> JoinAsync(RoomSource room, int audioPort, CancellationToken ct)
    {
        byte[]? clientKey = _store.LoadClientAuthKey(room.SourceId);
        if (clientKey == null) throw new InvalidOperationException("This room must be paired before connecting.");
        NetworkInterfaceSelection local = ResolveLocalAdapter(room);
        _control?.Dispose();
        _control = UdpSocketUtil.CreateBoundUdp(0, reuseAddress: false, localAddress: local.LocalAddress);
        UdpClient control = _control;
        IPEndPoint remote = RoomProtocol.Endpoint(room.SourceIp, room.ControlPort);
        UdpSocketUtil.ConnectUdp(control, remote);

        byte[] hello = new byte[88];
        BinaryPrimitives.WriteUInt32LittleEndian(hello.AsSpan(0, 4), RoomProtocol.Magic);
        hello[4] = RoomProtocol.Version;
        hello[5] = RoomProtocol.JoinHello;
        BinaryPrimitives.WriteUInt16LittleEndian(hello.AsSpan(6, 2), (ushort)hello.Length);
        room.SourceId.CopyTo(hello.AsSpan(8));
        RoomProtocol.WriteFixedString(hello.AsSpan(24, 9), room.RoomCode);
        _clientId.CopyTo(hello.AsSpan(33));
        byte[] nonce = CryptoUtil.RandomBytes(RoomProtocol.NonceLength);
        nonce.CopyTo(hello.AsSpan(49));
        BinaryPrimitives.WriteUInt16LittleEndian(hello.AsSpan(65, 2), (ushort)audioPort);
        hello[67] = RoomProtocol.CodecSbc;
        BinaryPrimitives.WriteUInt16LittleEndian(hello.AsSpan(68, 2), 48000);
        hello[70] = 2;
        hello[71] = 24;
        CryptoUtil.Cmac(clientKey, hello).CopyTo(hello.AsSpan(72));

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            await UdpSocketUtil.SendToAsync(control, hello, remote, ct);
            UdpReceiveResult? result = await UdpSocketUtil.ReceiveWithTimeoutAsync(control, TimeSpan.FromMilliseconds(700), ct);
            byte[]? accept = result?.Buffer;
            if (accept is null) continue;
            if (RoomProtocol.HasValidHeader(accept, RoomProtocol.JoinReject)) throw new UnauthorizedAccessException("Source rejected authentication.");
            if (!RoomProtocol.HasValidHeader(accept, RoomProtocol.JoinAccept) || accept.Length != 168) continue;
            if (!accept.AsSpan(8, 16).SequenceEqual(room.SourceId)) continue;
            if (!accept.AsSpan(57, 16).SequenceEqual(nonce)) continue;
            byte[] tagCopy = accept.ToArray();
            Array.Clear(tagCopy, 152, 16);
            if (!CryptoUtil.FixedEquals(CryptoUtil.Cmac(clientKey, tagCopy), accept.AsSpan(152, 16)))
                throw new UnauthorizedAccessException("JOIN_ACCEPT authentication failed.");
            byte[] sessionKey = CryptoUtil.AesCtr(clientKey, accept.AsSpan(89, 16).ToArray(), accept.AsSpan(105, 32));
            uint streamId = BinaryPrimitives.ReadUInt32LittleEndian(accept.AsSpan(33, 4));
            uint sessionId = BinaryPrimitives.ReadUInt32LittleEndian(accept.AsSpan(37, 4));
            uint startSeq = BinaryPrimitives.ReadUInt32LittleEndian(accept.AsSpan(148, 4));
            int frameBytes = BinaryPrimitives.ReadUInt16LittleEndian(accept.AsSpan(146, 2));
            int channels = accept[142];
            StartHeartbeat(room, clientKey, sessionId);
            return new AudioSession(sessionKey, streamId, sessionId, startSeq, frameBytes, channels);
        }
        throw new TimeoutException("No JOIN_ACCEPT received from source.");
    }

    public async Task LeaveAsync(RoomSource room, AudioSession session)
    {
        byte[]? key = _store.LoadClientAuthKey(room.SourceId);
        if (key == null) return;
        _heartbeatCts?.Cancel();
        await SendControlAsync(room, key, session.SessionId, RoomProtocol.Leave, 0, CancellationToken.None);
    }

    public Task IdentifyAsync(RoomSource room, AudioSession session, CancellationToken ct)
    {
        byte[]? key = _store.LoadClientAuthKey(room.SourceId);
        return key == null ? Task.CompletedTask : SendControlAsync(room, key, session.SessionId, RoomProtocol.Identify, 0, ct);
    }

    private void StartHeartbeat(RoomSource room, byte[] key, uint sessionId)
    {
        _heartbeatCts?.Cancel();
        _heartbeatCts = new CancellationTokenSource();
        _ = Task.Run(async () =>
        {
            uint counter = 0;
            while (!_heartbeatCts.IsCancellationRequested)
            {
                await SendControlAsync(room, key, sessionId, RoomProtocol.Heartbeat, ++counter, _heartbeatCts.Token);
                await Task.Delay(TimeSpan.FromSeconds(2), _heartbeatCts.Token);
            }
        }, _heartbeatCts.Token);
    }

    private async Task SendControlAsync(RoomSource room, byte[] key, uint sessionId, byte type, uint counter, CancellationToken ct)
    {
        byte[] packet = new byte[73];
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(0, 4), RoomProtocol.Magic);
        packet[4] = RoomProtocol.Version;
        packet[5] = type;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(6, 2), (ushort)packet.Length);
        room.SourceId.CopyTo(packet.AsSpan(8));
        RoomProtocol.WriteFixedString(packet.AsSpan(24, 9), room.RoomCode);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(33, 4), sessionId);
        _clientId.CopyTo(packet.AsSpan(37));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(53, 4), counter);
        CryptoUtil.Cmac(key, packet).CopyTo(packet.AsSpan(57, 16));
        IPEndPoint remote = RoomProtocol.Endpoint(room.SourceIp, room.ControlPort);
        if (_control == null)
            throw new InvalidOperationException("Control socket is not connected.");
        await UdpSocketUtil.SendToAsync(_control, packet, remote, ct);
    }

    public void Dispose()
    {
        _heartbeatCts?.Cancel();
        _control?.Dispose();
    }

    private static NetworkInterfaceSelection ResolveLocalAdapter(RoomSource room)
    {
        NetworkInterfaceSelection? local = NetworkInterfaceSelector.FindLocalIPv4ForRemote(room.SourceIp);
        if (local == null)
        {
            throw new InvalidOperationException(
                $"No local network adapter was found on the same subnet as {room.SourceIp}. " +
                "If using Windows Mobile Hotspot, make sure the ESP32 is connected to the hotspot and the hotspot adapter is active.");
        }
        room.LocalAdapterIp = local.LocalAddress;
        room.LocalAdapterName = local.AdapterName;
        return local;
    }
}
