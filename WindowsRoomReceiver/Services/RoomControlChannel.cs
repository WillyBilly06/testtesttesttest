using System.Net;
using System.Net.Sockets;

namespace WindowsRoomReceiver.Services;

/// <summary>
/// Lightweight UDP control channel built directly on <see cref="Socket"/>.
///
/// Why not <see cref="UdpClient"/> with <c>Connect</c>?
///   The previous implementations using <c>UdpClient.Connect</c> + connected
///   <c>SendAsync</c> kept tripping <c>WSAEOPNOTSUPP (10045)</c> on Windows
///   Mobile Hotspot adapters. The most robust path on every Windows
///   configuration is a raw <see cref="Socket"/> bound to <see cref="IPAddress.Any"/>
///   on an ephemeral port that uses <see cref="Socket.SendToAsync(System.ReadOnlyMemory{byte},SocketFlags,EndPoint,CancellationToken)"/>
///   and <see cref="Socket.ReceiveFromAsync(System.Memory{byte},SocketFlags,EndPoint,CancellationToken)"/>.
///
/// The channel is single-remote: every send goes to the constructor's
/// endpoint, and every receive accepts datagrams from any peer (the caller
/// validates the source).
/// </summary>
public sealed class RoomControlChannel : IDisposable
{
    private readonly Socket _socket;
    private readonly IPEndPoint _remote;

    public IPEndPoint Remote => _remote;
    public IPEndPoint LocalEndPoint => (IPEndPoint)_socket.LocalEndPoint!;

    public RoomControlChannel(IPEndPoint remote)
    {
        _remote = remote;
        _socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try { _socket.ExclusiveAddressUse = false; } catch (SocketException) { }
        _socket.Bind(new IPEndPoint(IPAddress.Any, 0));
    }

    public async Task SendAsync(byte[] packet, CancellationToken ct)
    {
        await _socket.SendToAsync(new ArraySegment<byte>(packet), SocketFlags.None, _remote).ConfigureAwait(false);
        ct.ThrowIfCancellationRequested();
    }

    public async Task<byte[]?> ReceiveAsync(TimeSpan timeout, CancellationToken ct)
    {
        byte[] buffer = new byte[2048];
        EndPoint from = new IPEndPoint(IPAddress.Any, 0);

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(timeout);

        try
        {
            var receiveTask = _socket.ReceiveFromAsync(new ArraySegment<byte>(buffer), SocketFlags.None, from);
            var timeoutTask = Task.Delay(Timeout.InfiniteTimeSpan, cts.Token);
            Task completed = await Task.WhenAny(receiveTask, timeoutTask).ConfigureAwait(false);
            if (completed != receiveTask)
            {
                ct.ThrowIfCancellationRequested();
                return null;
            }
            SocketReceiveFromResult result = await receiveTask.ConfigureAwait(false);
            if (result.ReceivedBytes == 0) return null;
            byte[] data = new byte[result.ReceivedBytes];
            Buffer.BlockCopy(buffer, 0, data, 0, result.ReceivedBytes);
            return data;
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            return null;
        }
    }

    public void Dispose()
    {
        try { _socket.Shutdown(SocketShutdown.Both); } catch { }
        _socket.Dispose();
    }
}
