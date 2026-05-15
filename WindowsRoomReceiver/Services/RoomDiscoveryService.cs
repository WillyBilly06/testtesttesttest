using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public sealed class RoomDiscoveryService
{
    private readonly SecureCredentialStore _store;
    private readonly ConcurrentDictionary<string, RoomSource> _rooms = new();

    public RoomDiscoveryService(SecureCredentialStore store)
    {
        _store = store;
    }

    public event Action<IReadOnlyCollection<RoomSource>>? RoomsChanged;

    public async Task RunAsync(CancellationToken ct)
    {
        using var udp = UdpSocketUtil.CreateBoundUdp(RoomProtocol.DiscoveryPort, enableBroadcast: true);
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result = await UdpSocketUtil.ReceiveAsyncNoCancelSocketOp(udp, ct);
            if (TryParseAdvertisement(result.Buffer, result.RemoteEndPoint.Address, out RoomSource? room) && room is not null)
            {
                room.IsPaired = _store.LoadClientAuthKey(room.SourceId) != null;
                room.State = room.IsPaired ? RoomConnectionState.Available :
                    room.PairingAvailable ? RoomConnectionState.PairingAvailable : RoomConnectionState.PairingRequired;
                _rooms[Convert.ToHexString(room.SourceId)] = room;
                RaiseRoomsChanged();
            }
            MarkStaleRooms();
        }
    }

    private bool TryParseAdvertisement(byte[] data, IPAddress sender, out RoomSource? room)
    {
        room = null;
        if (sender.AddressFamily != AddressFamily.InterNetwork) return false;
        if (!RoomProtocol.HasValidHeader(data, RoomProtocol.RoomAdvertise) || data.Length < 118) return false;
        byte[] sourceId = data.AsSpan(8, RoomProtocol.SourceIdLength).ToArray();
        string roomCode = RoomProtocol.FixedString(data.AsSpan(24, 9));
        string roomName = RoomProtocol.FixedString(data.AsSpan(33, 32));
        uint streamId = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(65, 4));
        ushort controlPort = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(73, 2));
        ushort sampleRate = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(75, 2));
        byte codec = data[81];
        byte channels = data[82];
        bool pairingAvailable = data[85] != 0;
        if (codec != RoomProtocol.CodecSbc) return false;
        NetworkInterfaceSelection? local = NetworkInterfaceSelector.FindLocalIPv4ForRemote(sender);
        room = new RoomSource
        {
            SourceId = sourceId,
            RoomCode = roomCode,
            RoomName = string.IsNullOrWhiteSpace(roomName) ? roomCode : roomName,
            SourceIp = sender,
            LocalAdapterIp = local?.LocalAddress,
            LocalAdapterName = local?.AdapterName ?? "No matching adapter",
            ControlPort = controlPort == 0 ? RoomProtocol.ControlPort : controlPort,
            StreamId = streamId,
            CodecInfo = $"SBC · {sampleRate / 1000.0:0.#} kHz · {channels} ch",
            PairingAvailable = pairingAvailable,
            LastSeen = DateTimeOffset.UtcNow
        };
        return true;
    }

    private void MarkStaleRooms()
    {
        DateTimeOffset cutoff = DateTimeOffset.UtcNow.AddSeconds(-8);
        foreach (RoomSource room in _rooms.Values)
        {
            if (room.LastSeen < cutoff) room.State = RoomConnectionState.Offline;
        }
    }

    private void RaiseRoomsChanged() => RoomsChanged?.Invoke(_rooms.Values.OrderBy(r => r.RoomName).ToArray());
}
