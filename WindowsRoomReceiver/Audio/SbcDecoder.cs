using System.IO;
using System.Runtime.InteropServices;

namespace WindowsRoomReceiver.Audio;

public sealed class SbcDecoder
{
    public bool IsAvailable
    {
        get
        {
            try
            {
                _ = sbc_decoder_probe();
                return true;
            }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
        }
    }

    public short[] Decode(ReadOnlySpan<byte> sbcFrame)
    {
        short[] pcm = new short[2048];
        int samples = sbc_decode_frame(sbcFrame.ToArray(), sbcFrame.Length, pcm, pcm.Length);
        if (samples <= 0) throw new InvalidDataException("SBC decode failed.");
        Array.Resize(ref pcm, samples);
        return pcm;
    }

    [DllImport("sbc_decoder_native", CallingConvention = CallingConvention.Cdecl)]
    private static extern int sbc_decoder_probe();

    [DllImport("sbc_decoder_native", CallingConvention = CallingConvention.Cdecl)]
    private static extern int sbc_decode_frame(byte[] input, int inputLen, short[] output, int maxSamples);
}
