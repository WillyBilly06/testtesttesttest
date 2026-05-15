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

    public static UdpClient CreateMulticastReceiver(IPAddress groupAddress, int port, IPAddress localInterfaceAddress)
    {
        if (!IsMulticastIPv4(groupAddress))
            throw new ArgumentException($"{groupAddress} is not an IPv4 multicast address.", nameof(groupAddress));
        if (localInterfaceAddress.AddressFamily != AddressFamily.InterNetwork)
            throw new ArgumentException("The multicast interface address must be IPv4.", nameof(localInterfaceAddress));

        UdpClient udp = CreateUnboundUdp(reuseAddress: true);

        // For multicast receive on Windows, bind to Any:port, then join the group on
        // the selected local adapter. Binding directly to the multicast address or the
        // wrong adapter can fail when the PC has Ethernet + Wi-Fi hotspot enabled.
        udp.Client.Bind(new IPEndPoint(IPAddress.Any, port));
        udp.Client.SetSocketOption(
            SocketOptionLevel.IP,
            SocketOptionName.AddMembership,
            new MulticastOption(groupAddress, localInterfaceAddress));

        return udp;
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

        // Some Windows/network-driver combinations reject optional socket options with
        // WSAEOPNOTSUPP (10045). These options are useful but not required for the
        // ephemeral pairing/auth sockets, so keep them best-effort.
        try { udp.Client.ExclusiveAddressUse = false; } catch (SocketException) { }
        if (reuseAddress)
        {
            try { udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true); }
            catch (SocketException) { }
        }
        return udp;
    }

    public static void ConnectUdp(UdpClient udp, IPEndPoint remote)
    {
        udp.Client.Connect(remote);
    }

    public static Task SendToAsync(UdpClient udp, byte[] packet, IPEndPoint remote, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return Task.Run(() =>
        {
            ct.ThrowIfCancellationRequested();
            if (udp.Client.Connected)
            {
                udp.Client.Send(packet);
            }
            else
            {
                udp.Client.SendTo(packet, SocketFlags.None, remote);
            }
        }, ct);
    }

    public static async Task<UdpReceiveResult> ReceiveAsyncNoCancelSocketOp(UdpClient udp, CancellationToken ct)
    {
        // Use the classic ReceiveAsync() overload, not ReceiveAsync(ct). Some Windows
        // builds/drivers reject the cancellable socket operation with WSAEOPNOTSUPP.
        Task<UdpReceiveResult> receiveTask = udp.ReceiveAsync();
        Task cancelTask = Task.Delay(Timeout.InfiniteTimeSpan, ct);
        Task finished = await Task.WhenAny(receiveTask, cancelTask).ConfigureAwait(false);
        ct.ThrowIfCancellationRequested();
        return await receiveTask.ConfigureAwait(false);
    }

    public static Task<UdpReceiveResult?> ReceiveWithTimeoutAsync(UdpClient udp, TimeSpan timeout, CancellationToken ct)
    {
        // Use a connected UDP Receive() for pairing/auth when possible. It avoids
        // Poll/ReceiveFrom combinations that some Windows Wi-Fi drivers reject.
        return Task.Run(() =>
        {
            ct.ThrowIfCancellationRequested();
            Socket socket = udp.Client;
            int ms = timeout.TotalMilliseconds >= int.MaxValue
                ? int.MaxValue
                : Math.Max(1, (int)timeout.TotalMilliseconds);
            int saved = socket.ReceiveTimeout;
            try
            {
                socket.ReceiveTimeout = ms;
                byte[] buffer = new byte[2048];
                int len;
                IPEndPoint remote;
                if (socket.Connected)
                {
                    len = socket.Receive(buffer);
                    remote = (IPEndPoint)socket.RemoteEndPoint!;
                }
                else
                {
                    EndPoint any = new IPEndPoint(IPAddress.Any, 0);
                    len = socket.ReceiveFrom(buffer, ref any);
                    remote = (IPEndPoint)any;
                }

                if (len != buffer.Length) Array.Resize(ref buffer, len);
                return new UdpReceiveResult(buffer, remote);
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.TimedOut || ex.NativeErrorCode == 10060)
            {
                return (UdpReceiveResult?)null;
            }
            finally
            {
                try
                {
                    socket.ReceiveTimeout = saved;
                }
                catch (SocketException)
                {
                }
            }
        }, ct);
    }
}
