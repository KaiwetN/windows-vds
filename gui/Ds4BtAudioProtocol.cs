using System.Buffers.Binary;

namespace VdsGui;

/// <summary>
/// Wire format for DualShock 4 speaker audio over Bluetooth, as used by the
/// PS4 and documented in public protocol write-ups: a one-shot 0x11 control
/// report arms the audio plane and sets volumes, then 32 kHz SBC frames are
/// batched into 0x12/0x14/0x17 reports addressed to the internal speaker.
/// Byte positions below are fixed by the DS4 firmware.
/// </summary>
internal static class Ds4BtAudioProtocol
{
    public const int ControlReportLength = 78;
    public const int SbcFrameLength = 109;
    public const int SamplesPerChannel = 128;
    public const int SampleRate = 32_000;

    // SBC frame count per report ID: 0x12 = 1, 0x14 = 2, 0x17 = 4.
    public const int OneFrameReportLength = 142;
    public const int TwoFrameReportLength = 270;
    public const int FourFrameReportLength = 462;

    // Byte 5 of the speaker report selects the output: 0x02 internal speaker,
    // 0x24 headset jack.
    public const byte AudioTargetInternalSpeaker = 0x02;
    public const byte AudioTargetHeadset = 0x24;

    // Byte 2 selects the inbound report mode. 0xA0 keeps ordinary controller
    // input alive while the report ID/payload select speaker output; 0xA1 adds
    // microphone input during full duplex.
    public const byte AudioModeSpeaker = 0xA0;
    public const byte AudioModeDuplex = 0xA1;

    // Validity mask on the one-shot control report: 0xF3 arms audio, 0xF0
    // disarms it while keeping rumble/LED fields valid.
    private const byte ValidityAudioArmed = 0xF3;
    private const byte ValidityAudioDisarmed = 0xF0;

    private const byte CrcPrefix = 0xA2;

    /// <summary>
    /// Builds the 78-byte 0x11 report that enables (or disables) the DS4
    /// Bluetooth audio plane and sets headphone / speaker / mic volumes.
    /// </summary>
    public static byte[] BuildControlReport(
        bool speakerEnabled,
        int volumePercent,
        byte bluetoothPollRate = 4)
    {
        var report = new byte[ControlReportLength];
        report[0] = 0x11;
        report[1] = (byte)(0xC0 | Math.Min(bluetoothPollRate, (byte)16));
        report[2] = speakerEnabled ? AudioModeSpeaker : (byte)0x00;
        report[3] = speakerEnabled ? ValidityAudioArmed : ValidityAudioDisarmed;
        var volume = (byte)Math.Clamp(
            volumePercent * 0x7F / 100, 0, 0x7F);
        // Headphone L/R [21]/[22], mic [23], speaker [24]. Mic is always 0
        // here because this path does not open the controller microphone.
        report[21] = volume;
        report[22] = volume;
        report[23] = 0;
        report[24] = speakerEnabled ? volume : (byte)0;
        WriteCrc(report);
        return report;
    }

    /// <summary>
    /// Builds a speaker report carrying 1, 2 or 4 SBC frames (0x12/0x14/0x17).
    /// </summary>
    public static byte[] BuildSpeakerReport(
        ushort frameNumber,
        ReadOnlySpan<byte[]> frames,
        byte audioTarget = AudioTargetInternalSpeaker,
        byte bluetoothPollRate = 4)
    {
        var frameCount = frames.Length switch
        {
            1 => 1,
            2 => 2,
            4 => 4,
            _ => throw new ArgumentOutOfRangeException(
                nameof(frames), "a DS4 speaker report needs 1, 2 or 4 SBC frames")
        };
        var reportLength = frameCount switch
        {
            1 => OneFrameReportLength,
            2 => TwoFrameReportLength,
            _ => FourFrameReportLength
        };
        var report = new byte[reportLength];
        report[0] = frameCount switch
        {
            1 => (byte)0x12,
            2 => (byte)0x14,
            _ => (byte)0x17
        };
        // Low bits are the DS4 Bluetooth input interval; preserve it, else
        // the firmware resets the controller to rate zero during playback.
        report[1] = (byte)(0x40 | Math.Min(bluetoothPollRate, (byte)16));
        report[2] = AudioModeSpeaker;
        report[3] = (byte)frameNumber;
        report[4] = (byte)(frameNumber >> 8);
        report[5] = audioTarget;
        for (var index = 0; index < frameCount; index++)
        {
            frames[index].CopyTo(report, 6 + index * SbcFrameLength);
        }
        WriteCrc(report);
        return report;
    }

    /// <summary>
    /// Reflected CRC32 (poly 0xEDB88320) over the 0xA2 HIDP output prefix and
    /// the report bytes up to (but excluding) the CRC itself.
    /// </summary>
    private static void WriteCrc(byte[] report)
    {
        var crcOffset = report.Length - sizeof(uint);
        uint crc = 0xFFFFFFFFu;
        crc = UpdateCrc(crc, CrcPrefix);
        for (var index = 0; index < crcOffset; index++)
        {
            crc = UpdateCrc(crc, report[index]);
        }
        crc = ~crc;
        BinaryPrimitives.WriteUInt32LittleEndian(
            report.AsSpan(crcOffset, sizeof(uint)), crc);
    }

    private static uint UpdateCrc(uint crc, byte value)
    {
        crc ^= value;
        for (var bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u);
        }
        return crc;
    }
}
