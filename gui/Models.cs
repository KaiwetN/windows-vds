using System.Text.Json.Serialization;

namespace VdsGui;

public enum VdsServiceState
{
    NotInstalled,
    Stopped,
    StartPending,
    StopPending,
    Running,
    Unknown
}

public sealed record SystemSnapshot(
    VdsServiceState ServiceState,
    bool UsbipInstalled,
    bool HidHideInstalled,
    bool UpdateAvailable,
    bool SourceAvailable,
    string VdsctlPath);

public sealed class ControllerTarget
{
    [JsonPropertyName("address")]
    public string Address { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("registered")]
    public bool Registered { get; set; }
}

public sealed class ControllerStatus
{
    [JsonPropertyName("address")]
    public string Address { get; set; } = "";

    [JsonPropertyName("connected")]
    public bool Connected { get; set; }

    [JsonPropertyName("path")]
    public string Path { get; set; } = "";

    [JsonPropertyName("profile")]
    public string Profile { get; set; } = "";

    [JsonPropertyName("ports")]
    public int[] Ports { get; set; } = [];
}

public sealed class ControlReply
{
    [JsonPropertyName("OK")]
    public bool Ok { get; set; }

    [JsonPropertyName("error")]
    public string Error { get; set; } = "";
}

public sealed class AudioBufferReply
{
    [JsonPropertyName("OK")]
    public bool Ok { get; set; }

    [JsonPropertyName("error")]
    public string Error { get; set; } = "";

    [JsonPropertyName("chunks")]
    public int Chunks { get; set; }

    [JsonPropertyName("milliseconds")]
    public int Milliseconds { get; set; }
}

public sealed class ControllerRow
{
    public string Name { get; init; } = "DualSense";
    public string Address { get; init; } = "";
    public bool Online { get; init; }
    public bool Registered { get; init; }
    public bool Connected { get; init; }
    public string Endpoint { get; init; } = "";
    public string Profile { get; init; } = "";
    public string BluetoothStatus => Online ? "在线" : "离线";
    public string RegistrationStatus => Registered ? "已注册" : "未注册";
    public string VirtualStatus => Connected ? "有线模式" : "未建立";
    public string EndpointDisplay => string.IsNullOrWhiteSpace(Endpoint) ? "—" : Endpoint;
    public string ProfileDisplay => Profile switch
    {
        "ds5" => "DualSense",
        "dse" => "DualSense Edge",
        _ => "自动"
    };
}
