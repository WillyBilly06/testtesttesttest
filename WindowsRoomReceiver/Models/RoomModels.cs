using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Net;
using System.Runtime.CompilerServices;

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

public sealed class RoomSource : INotifyPropertyChanged
{
    public required byte[] SourceId { get; init; }
    public required string RoomCode { get; init; }

    private string _roomName = "Room";
    public string RoomName { get => _roomName; set => Set(ref _roomName, value); }

    private IPAddress _sourceIp = IPAddress.None;
    public IPAddress SourceIp { get => _sourceIp; set => Set(ref _sourceIp, value); }

    public IPAddress? LocalAdapterIp { get; set; }
    public string LocalAdapterName { get; set; } = "No matching adapter";
    public int ControlPort { get; set; }
    public uint StreamId { get; set; }

    private string _codecInfo = "SBC · 48 kHz";
    public string CodecInfo { get => _codecInfo; set => Set(ref _codecInfo, value); }

    public bool PairingAvailable { get; set; }

    private bool _isPaired;
    public bool IsPaired
    {
        get => _isPaired;
        set => Set(ref _isPaired, value);
    }

    public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;

    private RoomConnectionState _state = RoomConnectionState.Available;
    public RoomConnectionState State
    {
        get => _state;
        set
        {
            if (Set(ref _state, value))
            {
                OnPropertyChanged(nameof(StatusText));
            }
        }
    }

    public string StatusText => State switch
    {
        RoomConnectionState.Connected => "Connected",
        RoomConnectionState.Connecting => "Connecting…",
        RoomConnectionState.Authenticating => "Authenticating…",
        RoomConnectionState.PairingAvailable => "Available",
        RoomConnectionState.PairingRequired => "Pairing required",
        RoomConnectionState.Offline => "Offline",
        RoomConnectionState.AuthenticationFailed => "Authentication failed",
        RoomConnectionState.ConnectionLost => "Reconnecting…",
        _ => "Available"
    };

    // ---- Inline pairing UI state -----------------------------------------

    private bool _isPairing;
    public bool IsPairing { get => _isPairing; set => Set(ref _isPairing, value); }

    private string _pinInput = string.Empty;
    public string PinInput
    {
        get => _pinInput;
        set => Set(ref _pinInput, value ?? string.Empty);
    }

    private string _pairingStatus = string.Empty;
    public string PairingStatus { get => _pairingStatus; set => Set(ref _pairingStatus, value); }

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(name);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
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
