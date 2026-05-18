using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public enum SessionStatus
{
    Idle,
    Connecting,
    Connected,
    Reconnecting
}

public sealed record SessionStatusUpdate(SessionStatus Status, string Message);

/// <summary>
/// Sticky connection lifecycle. Once <see cref="StartAsync"/> succeeds, this
/// keeps the audio receiver bound and the auth/heartbeat alive forever — if
/// the source drops the client (heartbeat starvation, NVS stall, Wi-Fi
/// blip, source restart) the manager silently re-JOINs in the background.
///
/// The audio UDP socket and the playback device are created once and reused
/// across re-joins; only the per-session AES key / stream id are swapped.
/// The user only ever sees a brief "Reconnecting…" until audio resumes.
///
/// The manager is stopped only by <see cref="StopAsync"/> (Disconnect or
/// app close).
/// </summary>
public sealed class RoomSessionManager : IDisposable
{
    private readonly RoomSource _room;
    private readonly SecureCredentialStore _store;
    private readonly int _outputDeviceId;
    private readonly NetworkInterfaceSelection _local;

    private AudioReceiver? _receiver;
    private AuthClient? _auth;
    private CancellationTokenSource? _audioCts;
    private CancellationTokenSource? _supervisorCts;
    private AudioSession? _session;
    private SessionStatus _status = SessionStatus.Idle;
    private DateTime _lastJoinAttempt = DateTime.MinValue;

    public AudioReceiver? Receiver => _receiver;
    public RoomSource Room => _room;
    public SessionStatus Status => _status;

    public event Action<SessionStatusUpdate>? StatusChanged;

    public RoomSessionManager(RoomSource room, SecureCredentialStore store, int outputDeviceId, NetworkInterfaceSelection local)
    {
        _room = room;
        _store = store;
        _outputDeviceId = outputDeviceId;
        _local = local;
    }

    /// <summary>
    /// Performs the first JOIN, starts audio playback, and spawns a
    /// background supervisor that keeps the session alive forever.
    /// </summary>
    public async Task StartAsync(CancellationToken ct)
    {
        UpdateStatus(SessionStatus.Connecting, $"Authenticating with {_room.RoomName}…");

        _receiver = new AudioReceiver(_local.LocalAddress, outputDeviceId: _outputDeviceId);
        _audioCts = new CancellationTokenSource();
        _ = Task.Run(() => _receiver.RunAsync(_audioCts.Token));

        _auth = new AuthClient(_store);
        _auth.HeartbeatTrouble += msg => UpdateStatus(SessionStatus.Reconnecting, msg);

        await JoinOnceAsync(ct);
        if (_session == null)
        {
            await StopAsync();
            throw new TimeoutException("The source did not respond when joining.");
        }

        UpdateStatus(SessionStatus.Connected, $"Connected to {_room.RoomName}.");

        _supervisorCts = new CancellationTokenSource();
        _ = Task.Run(() => SupervisorLoopAsync(_supervisorCts.Token));
    }

    /// <summary>
    /// Tear down everything. The manager cannot be reused after this.
    /// </summary>
    public async Task StopAsync()
    {
        _supervisorCts?.Cancel();
        _audioCts?.Cancel();
        if (_auth != null && _session != null)
        {
            try { await _auth.LeaveAsync(_room, _session); } catch { /* best effort */ }
        }
        _auth?.Dispose();
        _receiver?.Dispose();
        _auth = null;
        _receiver = null;
        _session = null;
        UpdateStatus(SessionStatus.Idle, "Disconnected.");
    }

    public void Dispose() => _ = StopAsync();

    /// <summary>
    /// Background watchdog: if no audio packet arrives for a few seconds,
    /// silently re-JOIN so the user keeps experiencing a sticky connection.
    /// </summary>
    private async Task SupervisorLoopAsync(CancellationToken ct)
    {
        const int audioStallSeconds = 5;     // silence threshold before we suspect a drop
        const int rejoinIntervalMs = 2500;   // delay between re-join attempts

        while (!ct.IsCancellationRequested)
        {
            try { await Task.Delay(1000, ct); }
            catch (OperationCanceledException) { return; }

            if (_receiver == null || _auth == null) return;

            TimeSpan idle = DateTime.UtcNow - _receiver.LastPacketUtc;
            if (idle.TotalSeconds < audioStallSeconds) continue;

            // Audio has been silent — try to re-join.
            UpdateStatus(SessionStatus.Reconnecting,
                $"No audio for {(int)idle.TotalSeconds}s, reconnecting to {_room.RoomName}…");

            // Throttle so we never hammer the source faster than every
            // rejoinIntervalMs even if the timer ticks more often.
            DateTime now = DateTime.UtcNow;
            int elapsed = (int)(now - _lastJoinAttempt).TotalMilliseconds;
            if (elapsed < rejoinIntervalMs)
            {
                try { await Task.Delay(rejoinIntervalMs - elapsed, ct); }
                catch (OperationCanceledException) { return; }
            }

            try
            {
                await JoinOnceAsync(ct);
                if (_session != null)
                {
                    UpdateStatus(SessionStatus.Connected, $"Reconnected to {_room.RoomName}.");
                }
            }
            catch (UnauthorizedAccessException ex)
            {
                // Source explicitly refused us — most likely the saved key
                // has been forgotten on the source. Keep trying anyway in
                // case the source is mid-reboot.
                UpdateStatus(SessionStatus.Reconnecting,
                    $"Source refused authentication ({ex.Message}). Retrying…");
            }
            catch (Exception ex)
            {
                UpdateStatus(SessionStatus.Reconnecting,
                    $"Reconnect attempt failed: {ex.Message}. Retrying…");
            }
        }
    }

    private async Task JoinOnceAsync(CancellationToken ct)
    {
        if (_auth == null || _receiver == null) return;
        _lastJoinAttempt = DateTime.UtcNow;
        AudioSession session = await _auth.JoinAsync(_room, _receiver.Port, ct);
        _session = session;
        _receiver.SetSession(session);
    }

    private void UpdateStatus(SessionStatus status, string message)
    {
        _status = status;
        StatusChanged?.Invoke(new SessionStatusUpdate(status, message));
    }
}
