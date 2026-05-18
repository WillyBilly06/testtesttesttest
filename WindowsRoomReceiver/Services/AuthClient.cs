using System.Buffers.Binary;
using System.Net;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public sealed record AudioSession(byte[] Key, uint StreamId, uint SessionId, uint StartSequence, int FrameBytes, int Channels);

public sealed class AuthClient : IDisposable
{
    private readonly SecureCredentialStore _store;
    private readonly byte[] _clientId;
    private RoomControlChannel? _control;
    private CancellationTokenSource? _heartbeatCts;

    /// <summary>
    /// Raised when several consecutive heartbeats fail to send. Listeners
    /// should treat this as a hint to attempt a re-join — the link is NOT
    /// torn down here, audio sockets stay open, and the user remains on
    /// the "Connected" experience until they explicitly Disconnect.
    /// </summary>
    public event Action<string>? HeartbeatTrouble;

    public AuthClient(SecureCredentialStore store)
    {
        _store = store;
        _clientId = store.GetOrCreateClientId();
    }

    public async Task<AudioSession> JoinAsync(RoomSource room, int audioPort, CancellationToken ct)
    {
        byte[]? clientKey = _store.LoadClientAuthKey(room.SourceId);
        if (clientKey == null) throw new InvalidOperationException("This room must be paired before connecting.");
        ResolveLocalAdapter(room);
        _control?.Dispose();
        _control = null;
        IPEndPoint remote = RoomProtocol.Endpoint(room.SourceIp, room.ControlPort);

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
            var channel = new RoomControlChannel(remote);
            bool keep = false;
            try
            {
                await channel.SendAsync(hello, ct);
                byte[]? accept = await channel.ReceiveAsync(TimeSpan.FromMilliseconds(700), ct);
                if (accept is null) continue;
                if (RoomProtocol.HasValidHeader(accept, RoomProtocol.JoinReject)) throw new UnauthorizedAccessException("Source rejected authentication. Try un-pairing and pairing again.");
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
                _control = channel;
                keep = true;
                StartHeartbeat(room, clientKey, sessionId);
                return new AudioSession(sessionKey, streamId, sessionId, startSeq, frameBytes, channels);
            }
            finally
            {
                if (!keep) channel.Dispose();
            }
        }
        throw new TimeoutException("No response from source while joining the room.");
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
        CancellationToken ct = _heartbeatCts.Token;
        _ = Task.Run(async () =>
        {
            uint counter = 0;
            int consecutiveFailures = 0;
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    await SendControlAsync(room, key, sessionId, RoomProtocol.Heartbeat, ++counter, ct);
                    consecutiveFailures = 0;
                }
                catch (OperationCanceledException) { return; }
                catch (Exception ex)
                {
                    consecutiveFailures++;
                    if (consecutiveFailures == 3 || consecutiveFailures % 8 == 0)
                    {
                        // Notify upper layer so it can trigger a re-join.
                        // Keep retrying heartbeats forever in the meantime —
                        // the user explicitly stays connected until they
                        // press Disconnect.
                        HeartbeatTrouble?.Invoke($"Heartbeat to source is failing ({ex.GetType().Name}).");
                    }
                }

                try { await Task.Delay(TimeSpan.FromSeconds(2), ct); }
                catch (OperationCanceledException) { return; }
            }
        }, ct);
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
        if (_control == null) throw new InvalidOperationException("Control channel is not connected.");
        await _control.SendAsync(packet, ct);
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
                $"No local network adapter was found on the same subnet as {room.SourceIp}.");
        }
        room.LocalAdapterIp = local.LocalAddress;
        room.LocalAdapterName = local.AdapterName;
        return local;
    }
}
