using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace WindowsRoomReceiver.Services;

public sealed record NetworkInterfaceSelection(
    IPAddress LocalAddress,
    IPAddress SubnetMask,
    string AdapterName,
    string AdapterDescription,
    NetworkInterfaceType InterfaceType)
{
    public bool LooksLikeWindowsHotspot =>
        LocalAddress.ToString().StartsWith("192.168.137.", StringComparison.Ordinal) ||
        AdapterName.Contains("hotspot", StringComparison.OrdinalIgnoreCase) ||
        AdapterDescription.Contains("hotspot", StringComparison.OrdinalIgnoreCase) ||
        AdapterName.Contains("Local Area Connection", StringComparison.OrdinalIgnoreCase);

    public string DisplayName => $"{LocalAddress} ({AdapterName})";
}

public static class NetworkInterfaceSelector
{
    public static NetworkInterfaceSelection? FindLocalIPv4ForRemote(IPAddress remoteIp)
    {
        if (remoteIp.AddressFamily != AddressFamily.InterNetwork)
            return null;

        NetworkInterfaceSelection? fallback = null;
        foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != OperationalStatus.Up)
                continue;
            if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                continue;

            IPInterfaceProperties props;
            try
            {
                props = ni.GetIPProperties();
            }
            catch
            {
                continue;
            }

            foreach (UnicastIPAddressInformation ua in props.UnicastAddresses)
            {
                if (ua.Address.AddressFamily != AddressFamily.InterNetwork)
                    continue;
                if (IPAddress.IsLoopback(ua.Address))
                    continue;

                IPAddress? mask = ua.IPv4Mask;
                if (mask == null)
                    continue;

                var selection = new NetworkInterfaceSelection(
                    ua.Address,
                    mask,
                    ni.Name,
                    ni.Description,
                    ni.NetworkInterfaceType);

                if (IsSameSubnet(remoteIp, ua.Address, mask))
                    return selection;

                fallback ??= selection;
            }
        }

        return fallback != null && IsLinkLocalCompatible(remoteIp, fallback.LocalAddress) ? fallback : null;
    }

    public static bool IsSameSubnet(IPAddress remoteIp, IPAddress localIp, IPAddress mask)
    {
        byte[] a = remoteIp.GetAddressBytes();
        byte[] b = localIp.GetAddressBytes();
        byte[] m = mask.GetAddressBytes();
        if (a.Length != 4 || b.Length != 4 || m.Length != 4)
            return false;

        for (int i = 0; i < 4; i++)
        {
            if ((a[i] & m[i]) != (b[i] & m[i]))
                return false;
        }

        return true;
    }

    private static bool IsLinkLocalCompatible(IPAddress remoteIp, IPAddress localIp)
    {
        byte[] r = remoteIp.GetAddressBytes();
        byte[] l = localIp.GetAddressBytes();
        return r.Length == 4 && l.Length == 4 && r[0] == 169 && r[1] == 254 && l[0] == 169 && l[1] == 254;
    }
}
