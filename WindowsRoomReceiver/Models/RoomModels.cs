using System.Collections.ObjectModel;
using System.Net;

namespace WindowsRoomReceiver.Models;

public enum RoomConnectionState
{
    Available,
    PairingRequired,
    PairingAvailable,
    Connecting,
    Authenticating,
    Connected,
    Offline,
    AuthenticationFailed,
    UnsupportedCodec,
    ConnectionLost
}

public sealed class RoomSource
{
    public required byte[] SourceId { get; init; }
    public required string RoomCode { get; init; }
    public string RoomName { get; set; } = "Room";
    public IPAddress SourceIp { get; set; } = IPAddress.None;
    public IPAddress? LocalAdapterIp { get; set; }
    public string LocalAdapterName { get; set; } = "No matching adapter";
    public int ControlPort { get; set; }
    public uint StreamId { get; set; }
    public string CodecInfo { get; set; } = "SBC · 48 kHz";
    public bool PairingAvailable { get; set; }
    public bool IsPaired { get; set; }
    public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;
    public RoomConnectionState State { get; set; } = RoomConnectionState.Available;
    public string StatusText => State switch
    {
        RoomConnectionState.Connected => "Connected",
        RoomConnectionState.PairingAvailable => "Pairing available",
        RoomConnectionState.PairingRequired => "Pairing required",
        RoomConnectionState.Offline => "Offline",
        RoomConnectionState.AuthenticationFailed => "Authentication failed",
        _ => "Available"
    };
    public string LocalNetworkText => LocalAdapterIp == null
        ? $"Source {SourceIp} · No matching local adapter"
        : $"Source {SourceIp} · Local adapter {LocalAdapterIp} · {LocalAdapterName}";
}

public sealed record ManualUdpEndpoint(IPAddress Address, int Port)
{
    public bool IsMulticast => Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork &&
                               Address.GetAddressBytes()[0] is >= 224 and <= 239;

    public string DisplayText => IsMulticast
        ? $"multicast {Address}:{Port}"
        : $"UDP {Address}:{Port}";
}

public sealed class PairedSource
{
    public required byte[] SourceId { get; init; }
    public required string RoomCode { get; init; }
    public string RoomName { get; set; } = "Room";
    public IPAddress LastKnownIp { get; set; } = IPAddress.None;
    public int ControlPort { get; set; } = 46000;
    public DateTimeOffset PairedAt { get; set; } = DateTimeOffset.UtcNow;
}

public sealed class AppState
{
    public ObservableCollection<RoomSource> Rooms { get; } = new();
    public RoomSource? ConnectedRoom { get; set; }
    public string Status { get; set; } = "Looking for rooms on your network...";
}
