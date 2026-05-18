using System.Net;
using System.Net.Sockets;

namespace WindowsRoomReceiver.Services;

internal static class UdpSocketUtil
{
    public static bool IsMulticastIPv4(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork) return false;
        byte first = address.GetAddressBytes()[0];
        return first >= 224 && first <= 239;
    }

    public static UdpClient CreateBoundUdp(int port, bool enableBroadcast = false, bool allowPortFallback = false, bool reuseAddress = true, IPAddress? localAddress = null)
    {
        localAddress ??= IPAddress.Any;
        UdpClient udp = CreateUnboundUdp(enableBroadcast, reuseAddress);
        try
        {
            udp.Client.Bind(new IPEndPoint(localAddress, port));
            return udp;
        }
        catch (SocketException) when (allowPortFallback && port != 0)
        {
            udp.Dispose();
            udp = CreateUnboundUdp(enableBroadcast, reuseAddress: false);
            udp.Client.Bind(new IPEndPoint(localAddress, 0));
            return udp;
        }
    }

    public static UdpClient CreateUnboundUdp(bool enableBroadcast = false, bool reuseAddress = false)
    {
        var udp = new UdpClient(AddressFamily.InterNetwork)
        {
            EnableBroadcast = enableBroadcast
        };
        try { udp.Client.ExclusiveAddressUse = false; } catch (SocketException) { }
        if (reuseAddress)
        {
            try { udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true); }
            catch (SocketException) { }
        }
        return udp;
    }

    /// <summary>
    /// Creates a UDP client connected to <paramref name="remote"/>. Windows
    /// selects the appropriate local interface and ephemeral port automatically;
    /// this avoids WSAEOPNOTSUPP (10045) errors that some Wi-Fi drivers raise
    /// when binding to a specific local IPv4 address before sending.
    /// </summary>
    public static UdpClient CreateConnectedUdp(IPEndPoint remote)
    {
        UdpClient udp = CreateUnboundUdp();
        udp.Client.Connect(remote);
        return udp;
    }

    public static async Task SendAsync(UdpClient udp, byte[] packet, IPEndPoint? fallbackRemote, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        if (udp.Client.Connected)
        {
            await udp.SendAsync(packet, packet.Length).ConfigureAwait(false);
        }
        else if (fallbackRemote != null)
        {
            await udp.SendAsync(packet, packet.Length, fallbackRemote).ConfigureAwait(false);
        }
        else
        {
            throw new InvalidOperationException("UDP socket is neither connected nor has a fallback remote endpoint.");
        }
    }

    public static async Task<UdpReceiveResult> ReceiveAsyncNoCancelSocketOp(UdpClient udp, CancellationToken ct)
    {
        Task<UdpReceiveResult> receiveTask = udp.ReceiveAsync();
        Task cancelTask = Task.Delay(Timeout.InfiniteTimeSpan, ct);
        Task finished = await Task.WhenAny(receiveTask, cancelTask).ConfigureAwait(false);
        ct.ThrowIfCancellationRequested();
        return await receiveTask.ConfigureAwait(false);
    }

    public static async Task<UdpReceiveResult?> ReceiveWithTimeoutAsync(UdpClient udp, TimeSpan timeout, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        Task<UdpReceiveResult> receiveTask = udp.ReceiveAsync();
        Task timeoutTask = Task.Delay(timeout, ct);
        Task completed = await Task.WhenAny(receiveTask, timeoutTask).ConfigureAwait(false);
        if (completed == receiveTask)
        {
            return await receiveTask.ConfigureAwait(false);
        }
        ct.ThrowIfCancellationRequested();
        return null;
    }
}
