using System.Runtime.InteropServices;

namespace WindowsRoomReceiver.Security;

internal static class Dpapi
{
    public static byte[] Protect(byte[] data) => Crypt(data, protect: true);
    public static byte[] Unprotect(byte[] data) => Crypt(data, protect: false);

    private static byte[] Crypt(byte[] data, bool protect)
    {
        DATA_BLOB input = DATA_BLOB.From(data);
        DATA_BLOB output = default;
        try
        {
            bool ok = protect
                ? CryptProtectData(ref input, null, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0, ref output)
                : CryptUnprotectData(ref input, null, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0, ref output);
            if (!ok) throw new InvalidOperationException("Windows secure storage failed.");
            byte[] result = new byte[output.cbData];
            Marshal.Copy(output.pbData, result, 0, result.Length);
            return result;
        }
        finally
        {
            input.Free();
            if (output.pbData != IntPtr.Zero) LocalFree(output.pbData);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DATA_BLOB
    {
        public int cbData;
        public IntPtr pbData;

        public static DATA_BLOB From(byte[] data)
        {
            IntPtr ptr = Marshal.AllocHGlobal(data.Length);
            Marshal.Copy(data, 0, ptr, data.Length);
            return new DATA_BLOB { cbData = data.Length, pbData = ptr };
        }

        public void Free()
        {
            if (pbData != IntPtr.Zero) Marshal.FreeHGlobal(pbData);
            pbData = IntPtr.Zero;
        }
    }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptProtectData(ref DATA_BLOB pDataIn, string? szDataDescr, IntPtr pOptionalEntropy,
        IntPtr pvReserved, IntPtr pPromptStruct, int dwFlags, ref DATA_BLOB pDataOut);

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptUnprotectData(ref DATA_BLOB pDataIn, string? ppszDataDescr, IntPtr pOptionalEntropy,
        IntPtr pvReserved, IntPtr pPromptStruct, int dwFlags, ref DATA_BLOB pDataOut);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr hMem);
}
