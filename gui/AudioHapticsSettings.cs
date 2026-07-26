using System.IO;
using System.Text.Json;

namespace VdsGui;

public sealed class AudioHapticsSettings
{
    public string DeviceId { get; set; } = "";
    public bool AutoStart { get; set; }
    public double StrengthPercent { get; set; } = 100;
    public double InputGainDb { get; set; } = 0;
    public double LowCutHz { get; set; } = 35;
    public double HighCutHz { get; set; } = 320;
    public double GateDb { get; set; } = -58;
    public double CompressionRatio { get; set; } = 2.5;
    public double AttackMs { get; set; } = 4;
    public double ReleaseMs { get; set; } = 120;
    public double StereoWidthPercent { get; set; } = 100;
    public double LeftPercent { get; set; } = 100;
    public double RightPercent { get; set; } = 100;
    public double CeilingPercent { get; set; } = 92;
    public string ChannelMode { get; set; } = "stereo";
    public bool InvertRight { get; set; }

    // --- DSX-model haptics shaping -------------------------------------
    // Implements the AudioToHaptics_Mix_Mode {OFF, BAND_3, BAND_6}. Uses
    // NAudio BiQuad: low shelf on the first band, peaking in the middle,
    // high shelf on the last. Gains are dB in a -20..+20 range.
    public string EqMode { get; set; } = "off";

    // Frequencies are tuned for this path rather than a reference (3-band 100/500/5k,
    // 6-band 50/120/400/1k/3k/10k). The haptics channel is decimated 16:1 to
    // 3 kHz, so its Nyquist is 1.5 kHz and every band above that is inert
    // here - those bands also feed the speaker and headset, which this
    // injection path does not carry. These land inside the band the voice
    // coils actually reproduce.
    public double Eq3Band1Hz { get; set; } = 60;
    public double Eq3Band1GainDb { get; set; }
    public double Eq3Band2Hz { get; set; } = 300;
    public double Eq3Band2GainDb { get; set; }
    public double Eq3Band3Hz { get; set; } = 900;
    public double Eq3Band3GainDb { get; set; }

    public double Eq6Band1Hz { get; set; } = 40;
    public double Eq6Band1GainDb { get; set; }
    public double Eq6Band2Hz { get; set; } = 80;
    public double Eq6Band2GainDb { get; set; }
    public double Eq6Band3Hz { get; set; } = 160;
    public double Eq6Band3GainDb { get; set; }
    public double Eq6Band4Hz { get; set; } = 320;
    public double Eq6Band4GainDb { get; set; }
    public double Eq6Band5Hz { get; set; } = 640;
    public double Eq6Band5GainDb { get; set; }
    public double Eq6Band6Hz { get; set; } = 1200;
    public double Eq6Band6GainDb { get; set; }

    // Haptics gain: linear multiplier, slider 0.1..10,
    // "Recommended: 1.0 - 5.0". Defaults to 1.0 here so existing profiles
    // keep their current loudness (a reference ships 2.0).
    public double HapticsGain { get; set; } = 1.0;

    // Source stereo channel mapping: the SOURCE
    // stereo channel feeding each actuator (0 = Left, 1 = Right).
    public int LeftMotorSource { get; set; }
    public int RightMotorSource { get; set; } = 1;

    // Controller speaker / headphone output: the raw capture (pre-haptics-DSP)
    // is sent on the stream's speaker channels. Only audible in native 0x36
    // haptics mode - the 0x31 rumble report has no audio block.
    public bool SpeakerEnabled { get; set; }
    public double SpeakerVolumePercent { get; set; } = 60;

    public double EqBandHz(int band) => EqMode switch
    {
        "band3" => band switch { 0 => Eq3Band1Hz, 1 => Eq3Band2Hz, _ => Eq3Band3Hz },
        "band6" => band switch
        {
            0 => Eq6Band1Hz,
            1 => Eq6Band2Hz,
            2 => Eq6Band3Hz,
            3 => Eq6Band4Hz,
            4 => Eq6Band5Hz,
            _ => Eq6Band6Hz
        },
        _ => 0
    };

    public double EqBandGainDb(int band) => EqMode switch
    {
        "band3" => band switch { 0 => Eq3Band1GainDb, 1 => Eq3Band2GainDb, _ => Eq3Band3GainDb },
        "band6" => band switch
        {
            0 => Eq6Band1GainDb,
            1 => Eq6Band2GainDb,
            2 => Eq6Band3GainDb,
            3 => Eq6Band4GainDb,
            4 => Eq6Band5GainDb,
            _ => Eq6Band6GainDb
        },
        _ => 0
    };

    public int EqBandCount => EqMode switch { "band3" => 3, "band6" => 6, _ => 0 };

    public AudioHapticsSettings Clone() => (AudioHapticsSettings)MemberwiseClone();

    public static AudioHapticsSettings Load()
    {
        try
        {
            var path = SettingsPath();
            if (!File.Exists(path))
            {
                return new AudioHapticsSettings();
            }
            var settings = JsonSerializer.Deserialize<AudioHapticsSettings>(File.ReadAllText(path))
                           ?? new AudioHapticsSettings();
            settings.NormalizeEqFrequencies();
            return settings;
        }
        catch
        {
            return new AudioHapticsSettings();
        }
    }

    /// <summary>
    /// Band centres are not user-editable, so any persisted value is simply an
    /// older default. Re-seed them so existing profiles inherit band centres
    /// that are inside the haptics passband.
    /// </summary>
    private void NormalizeEqFrequencies()
    {
        var defaults = new AudioHapticsSettings();
        Eq3Band1Hz = defaults.Eq3Band1Hz;
        Eq3Band2Hz = defaults.Eq3Band2Hz;
        Eq3Band3Hz = defaults.Eq3Band3Hz;
        Eq6Band1Hz = defaults.Eq6Band1Hz;
        Eq6Band2Hz = defaults.Eq6Band2Hz;
        Eq6Band3Hz = defaults.Eq6Band3Hz;
        Eq6Band4Hz = defaults.Eq6Band4Hz;
        Eq6Band5Hz = defaults.Eq6Band5Hz;
        Eq6Band6Hz = defaults.Eq6Band6Hz;
    }

    /// <summary>
    /// True when the chain is driven hard enough that the daemon's int8 clamp
    /// will distort. Common in profiles tuned before native voice-coil output,
    /// where extra gain was compensating for the lossy rumble conversion.
    /// </summary>
    public bool ExceedsSafeHeadroom =>
        StrengthPercent > 130 || InputGainDb > 6 ||
        LeftPercent > 130 || RightPercent > 130 || HapticsGain > 5;

    public void Save()
    {
        var path = SettingsPath();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(this, new JsonSerializerOptions
        {
            WriteIndented = true
        }));
        File.Move(temporary, path, true);
    }

    private static string SettingsPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "vDS",
        "audio-haptics.json");
}

public sealed record AudioRenderDevice(string Id, string Name, bool IsDefault)
{
    public string DisplayName => IsDefault ? $"{Name}（默认）" : Name;
}

public sealed record AudioHapticsLevels(float InputLeft, float InputRight, float OutputLeft, float OutputRight);
