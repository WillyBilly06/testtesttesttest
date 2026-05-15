# Room Audio Receiver

Professional Windows receiver for the ESP32 WROVER room audio source.

## Current capabilities

- Listens for UDP `ROOM_ADVERTISE` packets on port `45600`.
- Shows discovered rooms with pairing state, room code, IP, and SBC format.
- Pairs with a source using the 6-digit source Web UI PIN.
- Stores `client_id` and per-source `client_auth_key` with Windows DPAPI.
- Authenticates with `UDP_JOIN_HELLO` and verifies `UDP_JOIN_ACCEPT`.
- Unwraps the current `audio_session_key` in memory only.
- Receives encrypted send-and-forget UDP audio on port `47000`.
- Decrypts SBC payloads with the source packet nonce format.
- Sends authenticated heartbeat, leave, and identify control messages.

## Build notes

This project targets `net8.0-windows` with WPF and NAudio. The current machine has .NET runtimes only, not a .NET SDK, so install the .NET Desktop SDK before building:

```powershell
dotnet build
```

## SBC decoder backend

The app is wired to a native decoder adapter named `sbc_decoder_native.dll` with:

- `int sbc_decoder_probe(void)`
- `int sbc_decode_frame(uint8_t* input, int inputLen, int16_t* output, int maxSamples)`

Until that DLL is added beside the app executable, the receiver can discover, pair, authenticate, unwrap keys, receive, and decrypt packets, but it will not output decoded PCM.


## Manual UDP audio endpoint

The main window now includes a manual UDP audio endpoint section. Enable **Use**, enter an IPv4 address and port, then click **Apply**.

Example for the current ESP32 source multicast audio stream:

- IP/group: `239.10.10.10`
- Port: `5004`

If the IP is multicast (`224.0.0.0` through `239.255.255.255`), the app binds to the selected local adapter and joins the multicast group on that adapter. This is useful when the PC is on Ethernet while the ESP32 is connected through the Windows Wi-Fi hotspot.

The manual endpoint controls where the app listens for audio. Pairing/auth still uses the selected room/source control port.
