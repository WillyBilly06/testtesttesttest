using System.Buffers.Binary;
using System.Net;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Security;

namespace WindowsRoomReceiver.Services;

public sealed class PairingService
{
    private readonly SecureCredentialStore _store;
    private readonly byte[] _clientId;

    public PairingService(SecureCredentialStore store)
    {
        _store = store;
        _clientId = store.GetOrCreateClientId();
    }

    public async Task PairAsync(RoomSource room, string pin, string clientName, CancellationToken ct)
    {
        if (pin.Length != 6 || !pin.All(char.IsDigit)) throw new InvalidOperationException("Enter the 6-digit pairing PIN.");
        ResolveLocalAdapter(room);
        byte[] pinKey = CryptoUtil.DerivePinKey(room.SourceId, pin);
        byte[] packet = new byte[113];
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(0, 4), RoomProtocol.Magic);
        packet[4] = RoomProtocol.Version;
        packet[5] = RoomProtocol.PairRequest;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(6, 2), (ushort)packet.Length);
        room.SourceId.CopyTo(packet.AsSpan(8));
        RoomProtocol.WriteFixedString(packet.AsSpan(24, 9), room.RoomCode);
        _clientId.CopyTo(packet.AsSpan(33));
        RoomProtocol.WriteFixedString(packet.AsSpan(49, 32), clientName);
        byte[] nonce = CryptoUtil.RandomBytes(RoomProtocol.NonceLength);
        nonce.CopyTo(packet.AsSpan(81));
        CryptoUtil.Cmac(pinKey, packet).CopyTo(packet.AsSpan(97));

        IPEndPoint remote = RoomProtocol.Endpoint(room.SourceIp, room.ControlPort);
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            using var channel = new RoomControlChannel(remote);
            await channel.SendAsync(packet, ct);
            byte[]? response = await channel.ReceiveAsync(TimeSpan.FromMilliseconds(1200), ct);
            if (response is null) continue;
            if (RoomProtocol.HasValidHeader(response, RoomProtocol.PairReject))
                throw new InvalidOperationException("Source rejected pairing. Check that the PIN matches the room code on the source Web UI.");
            if (!RoomProtocol.HasValidHeader(response, RoomProtocol.PairAccept) || response.Length != 145) continue;
            byte[] authCopy = response.ToArray();
            Array.Clear(authCopy, 129, 16);
            byte[] expected = CryptoUtil.Cmac(pinKey, authCopy);
            if (!CryptoUtil.FixedEquals(expected, response.AsSpan(129, 16)))
                throw new InvalidOperationException("Pairing response authentication failed. The PIN may be incorrect.");
            byte[] wrappedKey = response.AsSpan(97, 32).ToArray();
            byte[] wrapNonce = response.AsSpan(81, 16).ToArray();
            byte[] clientAuthKey = CryptoUtil.AesCtr(pinKey, wrapNonce, wrappedKey);
            _store.SaveClientAuthKey(room.SourceId, clientAuthKey);
            var sources = _store.LoadPairedSources().Where(s => !s.SourceId.SequenceEqual(room.SourceId)).ToList();
            sources.Add(new PairedSource
            {
                SourceId = room.SourceId,
                RoomCode = room.RoomCode,
                RoomName = room.RoomName,
                LastKnownIp = room.SourceIp,
                ControlPort = room.ControlPort
            });
            _store.SavePairedSources(sources);
            return;
        }
        throw new TimeoutException("The source did not respond. Make sure it is on the same Wi-Fi network and try again.");
    }

    private static NetworkInterfaceSelection ResolveLocalAdapter(RoomSource room)
    {
        NetworkInterfaceSelection? local = NetworkInterfaceSelector.FindLocalIPv4ForRemote(room.SourceIp);
        if (local == null)
        {
            throw new InvalidOperationException(
                $"No local network adapter found on the same subnet as {room.SourceIp}. " +
                "If using Windows Mobile Hotspot, make sure the ESP32 is connected to the hotspot.");
        }
        room.LocalAdapterIp = local.LocalAddress;
        room.LocalAdapterName = local.AdapterName;
        return local;
    }
}
