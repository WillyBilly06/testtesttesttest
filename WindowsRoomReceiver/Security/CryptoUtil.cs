using System.Security.Cryptography;
using System.Text;

namespace WindowsRoomReceiver.Security;

public static class CryptoUtil
{
    public static byte[] RandomBytes(int len)
    {
        byte[] data = new byte[len];
        RandomNumberGenerator.Fill(data);
        return data;
    }

    public static byte[] DerivePinKey(byte[] sourceId, string pin)
    {
        using var sha = SHA256.Create();
        byte[] domain = Encoding.ASCII.GetBytes("room-audio-pin-v1");
        byte[] pinBytes = Encoding.ASCII.GetBytes(pin);
        byte[] input = new byte[domain.Length + sourceId.Length + pinBytes.Length];
        Buffer.BlockCopy(domain, 0, input, 0, domain.Length);
        Buffer.BlockCopy(sourceId, 0, input, domain.Length, sourceId.Length);
        Buffer.BlockCopy(pinBytes, 0, input, domain.Length + sourceId.Length, pinBytes.Length);
        return sha.ComputeHash(input);
    }

    public static byte[] AesCtr(byte[] key, byte[] nonce, ReadOnlySpan<byte> input)
    {
        using var aes = Aes.Create();
        aes.Mode = CipherMode.ECB;
        aes.Padding = PaddingMode.None;
        aes.Key = key;
        using ICryptoTransform enc = aes.CreateEncryptor();

        byte[] output = new byte[input.Length];
        byte[] counter = nonce.ToArray();
        byte[] block = new byte[16];
        int offset = 0;
        while (offset < input.Length)
        {
            enc.TransformBlock(counter, 0, 16, block, 0);
            int n = Math.Min(16, input.Length - offset);
            for (int i = 0; i < n; ++i) output[offset + i] = (byte)(input[offset + i] ^ block[i]);
            IncrementCounter(counter);
            offset += n;
        }
        return output;
    }

    public static byte[] Cmac(byte[] key, ReadOnlySpan<byte> data)
    {
        using var aes = Aes.Create();
        aes.Mode = CipherMode.ECB;
        aes.Padding = PaddingMode.None;
        aes.Key = key;
        using ICryptoTransform enc = aes.CreateEncryptor();
        byte[] zero = new byte[16];
        byte[] l = new byte[16];
        enc.TransformBlock(zero, 0, 16, l, 0);
        byte[] k1 = DoubleSubKey(l);
        byte[] k2 = DoubleSubKey(k1);
        int blocks = Math.Max(1, (data.Length + 15) / 16);
        bool complete = data.Length > 0 && data.Length % 16 == 0;
        byte[] x = new byte[16];
        byte[] mLast = new byte[16];
        ReadOnlySpan<byte> last = data.Slice((blocks - 1) * 16);
        if (complete)
        {
            last.CopyTo(mLast);
            XorInPlace(mLast, k1);
        }
        else
        {
            last.CopyTo(mLast);
            mLast[last.Length] = 0x80;
            XorInPlace(mLast, k2);
        }
        byte[] y = new byte[16];
        for (int b = 0; b < blocks - 1; ++b)
        {
            data.Slice(b * 16, 16).CopyTo(y);
            XorInPlace(y, x);
            enc.TransformBlock(y, 0, 16, x, 0);
        }
        XorInPlace(mLast, x);
        byte[] tag = new byte[16];
        enc.TransformBlock(mLast, 0, 16, tag, 0);
        return tag;
    }

    public static bool FixedEquals(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b) =>
        CryptographicOperations.FixedTimeEquals(a, b);

    private static void IncrementCounter(byte[] counter)
    {
        for (int i = counter.Length - 1; i >= 0; --i)
            if (++counter[i] != 0) break;
    }

    private static byte[] DoubleSubKey(byte[] input)
    {
        byte[] output = new byte[16];
        byte carry = 0;
        for (int i = 15; i >= 0; --i)
        {
            byte nextCarry = (byte)((input[i] & 0x80) != 0 ? 1 : 0);
            output[i] = (byte)((input[i] << 1) | carry);
            carry = nextCarry;
        }
        if (carry != 0) output[15] ^= 0x87;
        return output;
    }

    private static void XorInPlace(byte[] left, byte[] right)
    {
        for (int i = 0; i < left.Length; ++i) left[i] ^= right[i];
    }
}
