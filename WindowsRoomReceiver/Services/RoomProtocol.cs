using System.Buffers.Binary;
using System.Net;
using System.Text;

namespace WindowsRoomReceiver.Services;

public static class RoomProtocol
{
    public const uint Magic = 0x44554152;
    public const byte Version = 1;
    public const int DiscoveryPort = 45600;
    public const int ControlPort = 46000;
    public const int SourceIdLength = 16;
    public const int ClientIdLength = 16;
    public const int KeyLength = 32;
    public const int NonceLength = 16;
    public const int TagLength = 16;
    public const byte CodecSbc = 1;

    public const byte RoomAdvertise = 1;
    public const byte PairRequest = 10;
    public const byte PairAccept = 11;
    public const byte PairReject = 12;
    public const byte JoinHello = 20;
    public const byte JoinAccept = 21;
    public const byte JoinReject = 22;
    public const byte Heartbeat = 30;
    public const byte Leave = 31;
    public const byte Identify = 32;

    public static string FixedString(ReadOnlySpan<byte> data)
    {
        int len = data.IndexOf((byte)0);
        if (len < 0) len = data.Length;
        return Encoding.UTF8.GetString(data[..len]);
    }

    public static void WriteFixedString(Span<byte> dest, string value)
    {
        dest.Clear();
        byte[] raw = Encoding.UTF8.GetBytes(value);
        raw.AsSpan(0, Math.Min(raw.Length, dest.Length - 1)).CopyTo(dest);
    }

    public static bool HasValidHeader(ReadOnlySpan<byte> data, byte type)
    {
        return data.Length >= 8 &&
               BinaryPrimitives.ReadUInt32LittleEndian(data[0..4]) == Magic &&
               data[4] == Version &&
               data[5] == type &&
               BinaryPrimitives.ReadUInt16LittleEndian(data[6..8]) == data.Length;
    }

    public static IPEndPoint Endpoint(IPAddress address, int port)
    {
        if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6 &&
            address.IsIPv4MappedToIPv6)
        {
            address = address.MapToIPv4();
        }
        return new IPEndPoint(address, port);
    }
}
