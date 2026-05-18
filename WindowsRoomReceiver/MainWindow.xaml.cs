using System.Collections.ObjectModel;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using WindowsRoomReceiver.Audio;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;
using WindowsRoomReceiver.Services;

namespace WindowsRoomReceiver;

public partial class MainWindow : Window
{
    private readonly SecureCredentialStore _store = new();
    private readonly ObservableCollection<RoomSource> _rooms = new();
    private CancellationTokenSource _discoveryCts = new();
    private RoomDiscoveryService? _discovery;
    private RoomSource? _connectedRoom;
    private RoomSessionManager? _session;
    private int _selectedOutputDeviceId = AudioOutputDevice.WaveMapper;
    private readonly DispatcherTimer _watchdog;

    public MainWindow()
    {
        InitializeComponent();
        RoomsList.ItemsSource = _rooms;
        _rooms.CollectionChanged += (_, _) => UpdateRoomsHeader();
        UpdateRoomsHeader();
        LoadOutputDevices();

        _watchdog = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _watchdog.Tick += Watchdog_Tick;
        _watchdog.Start();

        StartDiscovery();
        ShowStoredPairedRooms();
    }

    // ====================================================================
    // Output devices
    // ====================================================================

    private void LoadOutputDevices()
    {
        var devices = AudioDeviceEnumerator.GetDevices();
        OutputDeviceCombo.ItemsSource = devices;
        OutputDeviceCombo.SelectedIndex = 0;
        _selectedOutputDeviceId = devices.Count > 0 ? devices[0].DeviceId : AudioOutputDevice.WaveMapper;
    }

    private void OutputDevice_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (OutputDeviceCombo.SelectedItem is AudioOutputDevice dev)
        {
            _selectedOutputDeviceId = dev.DeviceId;
            OutputDeviceHint.Text = _connectedRoom == null
                ? "Applies the next time you connect to a room."
                : "Disconnect and reconnect to switch output to this device.";
        }
    }

    // ====================================================================
    // Discovery + saved-room display
    // ====================================================================

    private void StartDiscovery()
    {
        _discoveryCts.Cancel();
        _discoveryCts = new CancellationTokenSource();
        _discovery = new RoomDiscoveryService(_store);
        _discovery.RoomsChanged += rooms =>
        {
            Dispatcher.Invoke(() =>
            {
                bool haveConnectedRoom = _connectedRoom != null && _session != null;
                MergeDiscoveredRooms(rooms);
                if (!haveConnectedRoom)
                {
                    StatusText.Text = _rooms.Count == 0
                        ? "No rooms detected. Make sure the source is online and on the same Wi-Fi as this PC."
                        : $"{_rooms.Count} room(s) discovered.";
                }
                UpdateRoomsHeader();
            });
        };
        _ = Task.Run(() => _discovery.RunAsync(_discoveryCts.Token));
    }

    private void RetryDiscovery_Click(object sender, RoutedEventArgs e) => StartDiscovery();

    /// <summary>
    /// Merges the latest advertisement set with what the UI already shows.
    /// We never blow the list away wholesale, so a user who is in the middle
    /// of typing a PIN does not lose their input on the next 1 s tick.
    /// Saved (paired-but-offline) rooms are also preserved.
    /// </summary>
    private void MergeDiscoveredRooms(IReadOnlyCollection<RoomSource> rooms)
    {
        // Index existing rooms by source id
        var existing = _rooms.ToDictionary(r => Convert.ToHexString(r.SourceId));

        foreach (RoomSource discovered in rooms)
        {
            string key = Convert.ToHexString(discovered.SourceId);
            if (existing.TryGetValue(key, out RoomSource? cur))
            {
                cur.RoomName = discovered.RoomName;
                cur.SourceIp = discovered.SourceIp;
                cur.LocalAdapterIp = discovered.LocalAdapterIp;
                cur.LocalAdapterName = discovered.LocalAdapterName;
                cur.ControlPort = discovered.ControlPort;
                cur.StreamId = discovered.StreamId;
                cur.CodecInfo = discovered.CodecInfo;
                cur.LastSeen = discovered.LastSeen;
                cur.IsPaired = _store.LoadClientAuthKey(cur.SourceId) != null;
                if (_connectedRoom != null && SameSource(cur, _connectedRoom))
                {
                    cur.State = RoomConnectionState.Connected;
                    _connectedRoom = cur;
                }
                else if (cur.State == RoomConnectionState.Offline)
                {
                    cur.State = RoomConnectionState.Available;
                }
                existing.Remove(key);
            }
            else
            {
                discovered.IsPaired = _store.LoadClientAuthKey(discovered.SourceId) != null;
                _rooms.Add(discovered);
            }
        }

        // What's left in `existing` was not in this advertisement burst.
        // Mark those rooms offline rather than removing them, so paired
        // rooms remain visible and forgettable.
        foreach (RoomSource stale in existing.Values)
        {
            if (DateTimeOffset.UtcNow - stale.LastSeen > TimeSpan.FromSeconds(8) &&
                (_connectedRoom == null || !SameSource(stale, _connectedRoom)))
            {
                stale.State = RoomConnectionState.Offline;
            }
        }
    }

    private void ShowStoredPairedRooms()
    {
        // On startup, surface any previously-paired rooms even before the
        // first advertisement arrives. They show as "Offline" until the
        // source is seen, but the user can already Forget them.
        foreach (PairedSource paired in _store.LoadPairedSources())
        {
            if (_rooms.Any(r => r.SourceId.SequenceEqual(paired.SourceId))) continue;
            _rooms.Add(new RoomSource
            {
                SourceId = paired.SourceId,
                RoomCode = paired.RoomCode,
                RoomName = string.IsNullOrWhiteSpace(paired.RoomName) ? paired.RoomCode : paired.RoomName,
                SourceIp = paired.LastKnownIp,
                ControlPort = paired.ControlPort,
                IsPaired = true,
                State = RoomConnectionState.Offline,
                CodecInfo = "SBC · 48 kHz · 2 ch",
                LastSeen = DateTimeOffset.UtcNow.AddSeconds(-30)
            });
        }
        UpdateRoomsHeader();
    }

    private void UpdateRoomsHeader()
    {
        EmptyHint.Visibility = _rooms.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        int online = _rooms.Count(r => r.State != RoomConnectionState.Offline);
        RoomCountText.Text = _rooms.Count switch
        {
            0 => "0 rooms",
            1 => $"{online}/1 online",
            _ => $"{online}/{_rooms.Count} online"
        };
    }

    // ====================================================================
    // Pairing — inline PIN entry per room
    // ====================================================================

    private void Pair_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        // Only one room can be in pairing mode at a time
        foreach (RoomSource other in _rooms)
        {
            if (!ReferenceEquals(other, room)) other.IsPairing = false;
        }
        room.PinInput = string.Empty;
        room.PairingStatus = $"PIN is shown on the source Web UI for {room.RoomName}.";
        room.IsPairing = true;
    }

    private void PinCancel_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        room.IsPairing = false;
        room.PinInput = string.Empty;
        room.PairingStatus = string.Empty;
    }

    private void PinBox_PreviewTextInput(object sender, TextCompositionEventArgs e)
    {
        // Only allow digits in the PIN entry box.
        if (!Regex.IsMatch(e.Text, "^[0-9]+$")) e.Handled = true;
    }

    private async void PinConfirm_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        string pin = (room.PinInput ?? string.Empty).Trim();
        if (pin.Length != 6 || !pin.All(char.IsDigit))
        {
            room.PairingStatus = "Enter the 6-digit PIN.";
            return;
        }
        try
        {
            room.PairingStatus = "Pairing…";
            await new PairingService(_store).PairAsync(room, pin, Environment.UserName, CancellationToken.None);
            room.IsPaired = true;
            room.IsPairing = false;
            room.PinInput = string.Empty;
            room.PairingStatus = string.Empty;
            StatusText.Text = $"Paired with {room.RoomName}.";
        }
        catch (Exception ex)
        {
            room.PairingStatus = FriendlyError(ex);
        }
    }

    private void Forget_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        var result = MessageBox.Show(
            $"Forget '{room.RoomName}'?\n\nThis removes the secure pairing key from this PC. " +
            "You will need the room's PIN to pair again later.",
            "Forget room", MessageBoxButton.OKCancel, MessageBoxImage.Question, MessageBoxResult.Cancel);
        if (result != MessageBoxResult.OK) return;

        _ = ForgetAsync(room);
    }

    private async Task ForgetAsync(RoomSource room)
    {
        if (_connectedRoom != null && SameSource(room, _connectedRoom))
        {
            await DisconnectInternalAsync(showStatus: false);
        }
        _store.ForgetSource(room.SourceId);
        room.IsPaired = false;
        room.IsPairing = false;
        room.PinInput = string.Empty;
        // If the room is offline-only (we just kept it around because it was
        // paired), removing the pairing should also remove the card.
        if (room.State == RoomConnectionState.Offline)
        {
            _rooms.Remove(room);
        }
        UpdateRoomsHeader();
        StatusText.Text = $"Removed '{room.RoomName}'.";
    }

    // ====================================================================
    // Connect / Identify / Disconnect
    // ====================================================================

    private async void Connect_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        try
        {
            await DisconnectInternalAsync(showStatus: false);
            NetworkInterfaceSelection local = ResolveLocalAdapter(room);
            room.State = RoomConnectionState.Connecting;
            StatusText.Text = $"Authenticating with {room.RoomName}…";

            var session = new RoomSessionManager(room, _store, _selectedOutputDeviceId, local);
            session.StatusChanged += update =>
            {
                Dispatcher.Invoke(() =>
                {
                    StatusText.Text = update.Message;
                    if (_connectedRoom == null) return;
                    switch (update.Status)
                    {
                        case SessionStatus.Connected:
                            _connectedRoom.State = RoomConnectionState.Connected;
                            HeaderConnectionText.Text = $"Connected • {_connectedRoom.RoomName}";
                            break;
                        case SessionStatus.Reconnecting:
                            _connectedRoom.State = RoomConnectionState.ConnectionLost;
                            HeaderConnectionText.Text = $"Reconnecting • {_connectedRoom.RoomName}";
                            break;
                        case SessionStatus.Connecting:
                            _connectedRoom.State = RoomConnectionState.Authenticating;
                            HeaderConnectionText.Text = $"Connecting • {_connectedRoom.RoomName}";
                            break;
                    }
                });
            };

            await session.StartAsync(CancellationToken.None);

            _session = session;
            _connectedRoom = room;
            room.State = RoomConnectionState.Connected;
            UpdateNowPlaying(room, local);
            DisconnectButton.IsEnabled = true;
            HeaderConnectionText.Text = $"Connected • {room.RoomName}";
        }
        catch (Exception ex)
        {
            await DisconnectInternalAsync(showStatus: false);
            room.State = RoomConnectionState.AuthenticationFailed;
            MessageBox.Show(FriendlyError(ex), "Connection failed", MessageBoxButton.OK, MessageBoxImage.Warning);
            StatusText.Text = "Connection failed.";
        }
    }

    private void Identify_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        if (_session == null || _connectedRoom == null || !SameSource(room, _connectedRoom))
        {
            MessageBox.Show("Connect to this room first.", "Identify", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        // Identify is best-effort and not a critical control path.
        StatusText.Text = $"Identify queued for {room.RoomName}.";
    }

    private async void Disconnect_Click(object sender, RoutedEventArgs e)
    {
        await DisconnectInternalAsync(showStatus: true);
    }

    private async Task DisconnectInternalAsync(bool showStatus)
    {
        if (_connectedRoom == null && _session == null) return;
        try
        {
            if (_session != null)
            {
                try { await _session.StopAsync(); } catch { /* best effort */ }
            }
        }
        finally
        {
            _session = null;
            string? roomName = _connectedRoom?.RoomName;
            if (_connectedRoom != null && _connectedRoom.State != RoomConnectionState.Offline)
                _connectedRoom.State = RoomConnectionState.Available;
            _connectedRoom = null;
            DisconnectButton.IsEnabled = false;
            NowPlayingRoom.Text = "Not connected";
            NowPlayingDetails.Text = "Connect to a paired room to begin streaming audio.";
            NowPlayingMetricsGrid.Visibility = Visibility.Collapsed;
            HeaderConnectionText.Text = "Not connected";
            if (showStatus)
            {
                StatusText.Text = roomName != null
                    ? $"Disconnected from {roomName}."
                    : "Disconnected.";
            }
        }
    }

    private void UpdateNowPlaying(RoomSource room, NetworkInterfaceSelection local)
    {
        NowPlayingRoom.Text = room.RoomName;
        string device = OutputDeviceCombo.SelectedItem is AudioOutputDevice d ? d.Name : "System Default Output";
        NowPlayingDetails.Text =
            $"{room.CodecInfo}\nSource {room.SourceIp}\nAdapter {local.AdapterName}\nOutput • {device}";
        NowPlayingMetricsGrid.Visibility = Visibility.Visible;
    }

    // ====================================================================
    // Watchdog: catch silent stream stalls (router blip, source crash)
    // ====================================================================

    private void Watchdog_Tick(object? sender, EventArgs e)
    {
        // Pure UI metric refresh. The session manager handles all
        // recovery work in its own background supervisor — this tick must
        // never tear the connection down on its own.
        if (_session?.Receiver is { } receiver && _connectedRoom != null)
        {
            MetricPackets.Text = receiver.PacketsReceived.ToString("N0");
            MetricBuffer.Text = $"{receiver.BufferMs} ms";
        }
    }

    // ====================================================================
    // Lifecycle / helpers
    // ====================================================================

    protected override async void OnClosed(EventArgs e)
    {
        _watchdog.Stop();
        _discoveryCts.Cancel();
        await DisconnectInternalAsync(showStatus: false);
        base.OnClosed(e);
    }

    private static string FriendlyError(Exception ex)
    {
        if (ex is System.Net.Sockets.SocketException socket)
        {
            return socket.SocketErrorCode switch
            {
                System.Net.Sockets.SocketError.AccessDenied =>
                    "Windows denied access to the UDP socket. Allow this app through Windows Defender Firewall and close any duplicate copies.",
                System.Net.Sockets.SocketError.AddressAlreadyInUse =>
                    "The UDP port is already in use. Close other copies of this app and try again.",
                System.Net.Sockets.SocketError.OperationNotSupported =>
                    "Windows refused this UDP operation. Check firewall or VPN settings and reconnect.",
                System.Net.Sockets.SocketError.TimedOut =>
                    "The source did not respond. Make sure it is on the same Wi-Fi.",
                _ => $"Network error: {socket.SocketErrorCode}. {socket.Message}"
            };
        }
        return ex.Message;
    }

    private static bool SameSource(RoomSource a, RoomSource b)
        => a.SourceId.SequenceEqual(b.SourceId);

    private static NetworkInterfaceSelection ResolveLocalAdapter(RoomSource room)
    {
        NetworkInterfaceSelection? local = NetworkInterfaceSelector.FindLocalIPv4ForRemote(room.SourceIp);
        if (local == null)
        {
            throw new InvalidOperationException(
                $"No local network adapter found on the same subnet as {room.SourceIp}.");
        }
        room.LocalAdapterIp = local.LocalAddress;
        room.LocalAdapterName = local.AdapterName;
        return local;
    }
}
