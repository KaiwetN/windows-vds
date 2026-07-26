using System.IO;
using System.Text.Json;

namespace VdsGui;

/// <summary>
/// GUI-side state for daemon-authored controller effects (adaptive triggers,
/// lightbar, player LEDs, mute LED). The daemon owns the applied state and
/// persists it in %ProgramData%\vDS\effects.json; this class only remembers
/// the panel's selections so they survive restarts and can be re-applied.
/// </summary>
public sealed class EffectsSettings
{
    public string LeftTrigger { get; set; } = "follow";
    public string RightTrigger { get; set; } = "follow";
    public int TriggerStrength { get; set; } = 6;
    public bool LightbarEnabled { get; set; }
    public int LightbarR { get; set; } = 0;
    public int LightbarG { get; set; } = 120;
    public int LightbarB { get; set; } = 255;
    public string PlayerLights { get; set; } = "follow";
    public string MuteLed { get; set; } = "follow";
    public bool Force { get; set; }

    /// <summary>
    /// Maps a GUI trigger preset onto a vdsctl trigger SPEC. "none" yields
    /// control back to the game; strength feeds the presets marked with it.
    /// </summary>
    public string TriggerSpec(string preset)
    {
        var strength = Math.Clamp(TriggerStrength, 1, 8);
        var machineStrength = Math.Min(7, strength);
        return preset switch
        {
            "off" => "off",
            "resistance" => $"feedback:0,{strength}",
            "endstop" => $"feedback:6,{strength}",
            "semiauto" => $"weapon:2,5,{strength}",
            "heavy" => $"weapon:2,7,{strength}",
            "vibrate" => $"vibration:0,{strength},22",
            "bow" => $"bow:1,6,{strength},6",
            "gallop" => "galloping:0,8,2,5,3",
            "machine" => $"machine:1,8,{machineStrength},{machineStrength},24,2",
            _ => "none"
        };
    }

    public IReadOnlyList<string> BuildVdsctlArguments()
    {
        var player = PlayerLights switch
        {
            "off" => "0",
            "p1" => "4",
            "p2" => "10",
            "p3" => "21",
            "p4" => "27",
            "all" => "31",
            _ => "none"
        };
        var mute = MuteLed switch
        {
            "off" => "off",
            "on" => "on",
            "breath" => "breath",
            _ => "none"
        };
        return
        [
            "--left-trigger", TriggerSpec(LeftTrigger),
            "--right-trigger", TriggerSpec(RightTrigger),
            "--led", LightbarEnabled
                ? $"{Math.Clamp(LightbarR, 0, 255)},{Math.Clamp(LightbarG, 0, 255)},{Math.Clamp(LightbarB, 0, 255)}"
                : "none",
            "--player", player,
            "--mute-led", mute,
            "--force", Force ? "on" : "off"
        ];
    }

    public static bool SettingsFileExists() => File.Exists(SettingsPath());

    public static EffectsSettings Load()
    {
        try
        {
            var path = SettingsPath();
            if (!File.Exists(path))
            {
                return new EffectsSettings();
            }
            return JsonSerializer.Deserialize<EffectsSettings>(File.ReadAllText(path))
                   ?? new EffectsSettings();
        }
        catch
        {
            return new EffectsSettings();
        }
    }

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
        "effects-ui.json");
}
