using System.Collections.ObjectModel;
using System.Net;
using System.Windows;
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
    private AudioSession? _session;
    private RoomSource? _connectedRoom;
    private AuthClient? _authClient;
    private AudioReceiver? _audioReceiver;
    private CancellationTokenSource? _audioCts;
    private ManualUdpEndpoint? _manualAudioEndpoint;

    public MainWindow()
    {
        InitializeComponent();
        RoomsList.ItemsSource = _rooms;
        ApplyManualUdpSettings(showMessage: false);
        StartDiscovery();
    }

    private void StartDiscovery()
    {
        _discoveryCts.Cancel();
        _discoveryCts = new CancellationTokenSource();
        _discovery = new RoomDiscoveryService(_store);
        _discovery.RoomsChanged += rooms =>
        {
            Dispatcher.Invoke(() =>
            {
                _rooms.Clear();
                foreach (RoomSource room in rooms) _rooms.Add(room);
                StatusText.Text = _rooms.Count == 0
                    ? "No rooms found. Make sure your source is powered on and connected to the same Wi-Fi."
                    : $"{_rooms.Count} room(s) found.";
            });
        };
        _ = Task.Run(() => _discovery.RunAsync(_discoveryCts.Token));
    }

    private void RetryDiscovery_Click(object sender, RoutedEventArgs e) => StartDiscovery();

    private void ApplyManualUdp_Click(object sender, RoutedEventArgs e)
    {
        ApplyManualUdpSettings(showMessage: true);
    }

    private void ApplyManualUdpSettings(bool showMessage)
    {
        if (UseManualUdpBox.IsChecked != true)
        {
            _manualAudioEndpoint = null;
            if (showMessage) StatusText.Text = "Manual UDP audio endpoint disabled. The app will use its normal audio receive port.";
            return;
        }

        if (!IPAddress.TryParse(ManualUdpIpBox.Text.Trim(), out IPAddress? address) ||
            address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
        {
            _manualAudioEndpoint = null;
            if (showMessage) MessageBox.Show("Enter a valid IPv4 address, for example 239.10.10.10.", "Invalid UDP IP", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!int.TryParse(ManualUdpPortBox.Text.Trim(), out int port) || port < 1 || port > 65535)
        {
            _manualAudioEndpoint = null;
            if (showMessage) MessageBox.Show("Enter a valid UDP port from 1 to 65535, for example 5004.", "Invalid UDP port", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        _manualAudioEndpoint = new ManualUdpEndpoint(address, port);
        if (showMessage) StatusText.Text = $"Manual UDP audio endpoint set to {_manualAudioEndpoint.DisplayText}.";
    }

    private async void Pair_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        string? pin = Prompt("Enter the 6-digit pairing PIN shown on the source Web UI:", "Pair Room");
        if (string.IsNullOrWhiteSpace(pin)) return;
        try
        {
            StatusText.Text = "Verifying pairing code and saving secure key...";
            await new PairingService(_store).PairAsync(room, pin.Trim(), Environment.UserName, CancellationToken.None);
            StatusText.Text = "Paired successfully. You can now connect.";
            room.IsPaired = true;
        }
        catch (Exception ex)
        {
            MessageBox.Show(FriendlyError(ex, room), "Pairing failed", MessageBoxButton.OK, MessageBoxImage.Warning);
            StatusText.Text = "Pairing failed.";
        }
    }

    private async void Connect_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room) return;
        try
        {
            ApplyManualUdpSettings(showMessage: false);
            NetworkInterfaceSelection local = ResolveLocalAdapter(room);
            string manualText = _manualAudioEndpoint == null ? "normal UDP audio port" : _manualAudioEndpoint.DisplayText;
            StatusText.Text = $"Authenticating and starting audio using {manualText}...";
            _authClient?.Dispose();
            _audioReceiver?.Dispose();
            _audioCts?.Cancel();
            _audioReceiver = new AudioReceiver(local.LocalAddress, manualEndpoint: _manualAudioEndpoint);
            _authClient = new AuthClient(_store);
            _session = await _authClient.JoinAsync(room, _audioReceiver.Port, CancellationToken.None);
            _audioCts = new CancellationTokenSource();
            _ = Task.Run(() => _audioReceiver.RunAsync(_session, _audioCts.Token));
            room.State = RoomConnectionState.Connected;
            _connectedRoom = room;
            StatusText.Text = _manualAudioEndpoint == null
                ? $"Connected. Audio bound to {local.LocalAddress}:{_audioReceiver.Port}."
                : $"Connected. Audio listening on {_manualAudioEndpoint.DisplayText} via adapter {local.LocalAddress}.";
        }
        catch (Exception ex)
        {
            MessageBox.Show(FriendlyError(ex, room), "Connection failed", MessageBoxButton.OK, MessageBoxImage.Warning);
            StatusText.Text = "Connection failed.";
        }
    }

    private async void Identify_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not RoomSource room || _session == null || _authClient == null) return;
        await _authClient.IdentifyAsync(room, _session, CancellationToken.None);
        StatusText.Text = "Identify sent.";
    }

    protected override async void OnClosed(EventArgs e)
    {
        _discoveryCts.Cancel();
        _audioCts?.Cancel();
        if (_connectedRoom != null && _session != null && _authClient != null)
            await _authClient.LeaveAsync(_connectedRoom, _session);
        _authClient?.Dispose();
        _audioReceiver?.Dispose();
        base.OnClosed(e);
    }

    private static string? Prompt(string text, string caption)
    {
        var dialog = new Window
        {
            Title = caption,
            Width = 420,
            Height = 180,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            ResizeMode = ResizeMode.NoResize
        };
        var box = new System.Windows.Controls.TextBox { Margin = new Thickness(0, 8, 0, 16), MaxLength = 6 };
        var ok = new System.Windows.Controls.Button { Content = "Continue", Width = 110, IsDefault = true };
        ok.Click += (_, _) => dialog.DialogResult = true;
        dialog.Content = new System.Windows.Controls.StackPanel
        {
            Margin = new Thickness(20),
            Children =
            {
                new System.Windows.Controls.TextBlock { Text = text, TextWrapping = TextWrapping.Wrap },
                box,
                ok
            }
        };
        return dialog.ShowDialog() == true ? box.Text : null;
    }

    private static string FriendlyError(Exception ex, RoomSource? room = null)
    {
        string network = room == null
            ? ""
            : $"\n\nSource IP: {room.SourceIp}\nLocal adapter: {room.LocalAdapterIp?.ToString() ?? "not selected"}\nAdapter name: {room.LocalAdapterName}\nControl port: {room.ControlPort}";
        if (ex is System.Net.Sockets.SocketException socket)
        {
            string message = socket.SocketErrorCode switch
            {
                System.Net.Sockets.SocketError.AccessDenied =>
                    "Windows denied access to the UDP socket. Allow this app through Windows Defender Firewall, close duplicate copies of the app, and make sure the source and PC are on the same Wi-Fi.",
                System.Net.Sockets.SocketError.AddressAlreadyInUse =>
                    "The UDP port is already in use. Close other copies of this app or any other program using the same audio/discovery port.",
                System.Net.Sockets.SocketError.OperationNotSupported =>
                    $"Windows rejected a UDP socket operation ({socket.NativeErrorCode}). This build now uses IPv4 UdpClient sockets; if this still happens, check firewall/VPN/network adapter restrictions.",
                System.Net.Sockets.SocketError.TimedOut =>
                    "The source did not respond in time. Make sure pairing mode is enabled and the PC is on the same Wi-Fi.",
                _ => $"Network error: {socket.SocketErrorCode} ({socket.NativeErrorCode}). {socket.Message}"
            };
            if (room?.LocalAdapterIp?.ToString().StartsWith("192.168.137.", StringComparison.Ordinal) == true)
            {
                message += "\n\nWindows Mobile Hotspot detected. Make sure Windows Defender Firewall allows this app on Private and Public networks.";
            }
            return message + network;
        }
        return ex.Message + network;
    }

    private static NetworkInterfaceSelection ResolveLocalAdapter(RoomSource room)
    {
        NetworkInterfaceSelection? local = NetworkInterfaceSelector.FindLocalIPv4ForRemote(room.SourceIp);
        if (local == null)
        {
            throw new InvalidOperationException(
                $"No local network adapter found on the same subnet as {room.SourceIp}.\n\n" +
                "If using Windows Mobile Hotspot, make sure the ESP32 is connected to the hotspot and the hotspot adapter is active.");
        }
        room.LocalAdapterIp = local.LocalAddress;
        room.LocalAdapterName = local.AdapterName;
        return local;
    }
}
