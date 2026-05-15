using System.IO;
using System.Text.Json;
using WindowsRoomReceiver.Models;
using WindowsRoomReceiver.Services;

namespace WindowsRoomReceiver.Security;

public sealed class SecureCredentialStore
{
    private readonly string _root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "RoomAudioReceiver");
    private readonly string _sourcesPath;
    private readonly string _clientIdPath;

    public SecureCredentialStore()
    {
        Directory.CreateDirectory(_root);
        _sourcesPath = Path.Combine(_root, "paired-sources.json");
        _clientIdPath = Path.Combine(_root, "client-id.bin");
    }

    public byte[] GetOrCreateClientId()
    {
        if (File.Exists(_clientIdPath))
        {
            return Dpapi.Unprotect(File.ReadAllBytes(_clientIdPath));
        }
        byte[] id = CryptoUtil.RandomBytes(RoomProtocol.ClientIdLength);
        File.WriteAllBytes(_clientIdPath, Dpapi.Protect(id));
        return id;
    }

    public IReadOnlyList<PairedSource> LoadPairedSources()
    {
        if (!File.Exists(_sourcesPath)) return Array.Empty<PairedSource>();
        return JsonSerializer.Deserialize<List<PairedSource>>(File.ReadAllText(_sourcesPath)) ?? new List<PairedSource>();
    }

    public void SavePairedSources(IEnumerable<PairedSource> sources)
    {
        File.WriteAllText(_sourcesPath, JsonSerializer.Serialize(sources, new JsonSerializerOptions { WriteIndented = true }));
    }

    public void SaveClientAuthKey(byte[] sourceId, byte[] key)
    {
        string path = SecretPath(sourceId);
        byte[] protectedKey = Dpapi.Protect(key);
        File.WriteAllBytes(path, protectedKey);
    }

    public byte[]? LoadClientAuthKey(byte[] sourceId)
    {
        string path = SecretPath(sourceId);
        if (!File.Exists(path)) return null;
        return Dpapi.Unprotect(File.ReadAllBytes(path));
    }

    public void ForgetSource(byte[] sourceId)
    {
        string path = SecretPath(sourceId);
        if (File.Exists(path)) File.Delete(path);
        SavePairedSources(LoadPairedSources().Where(s => !s.SourceId.SequenceEqual(sourceId)));
    }

    private string SecretPath(byte[] sourceId) => Path.Combine(_root, $"source-{Convert.ToHexString(sourceId)}.key");
}
