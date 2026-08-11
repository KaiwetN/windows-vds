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

    [JsonPropertyName("usb")]
    public bool Usb { get; set; }
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

public sealed class DeviceInfoReply
{
    [JsonPropertyName("OK")]
    public bool Ok { get; set; }

    [JsonPropertyName("controllers")]
    public DeviceInfoEntry[] Controllers { get; set; } = [];
}

public sealed class DeviceInfoEntry
{
    [JsonPropertyName("address")]
    public string Address { get; set; } = "";

    [JsonPropertyName("port")]
    public string Port { get; set; } = "";

    [JsonPropertyName("info")]
    public DeviceInfoDetails Info { get; set; } = new();
}

public sealed class DeviceInfoDetails
{
    [JsonPropertyName("model")]
    public string Model { get; set; } = "";

    [JsonPropertyName("serial")]
    public string Serial { get; set; } = "";

    [JsonPropertyName("firmware")]
    public string Firmware { get; set; } = "";

    [JsonPropertyName("hardware_version")]
    public string HardwareVersion { get; set; } = "";

    [JsonPropertyName("hardware_model")]
    public string HardwareModel { get; set; } = "";

    [JsonPropertyName("build_time")]
    public string BuildTime { get; set; } = "";

    [JsonPropertyName("color_name")]
    public string ColorName { get; set; } = "";

    [JsonPropertyName("mac_address")]
    public string MacAddress { get; set; } = "";

    [JsonPropertyName("left_module")]
    public string LeftModule { get; set; } = "";

    [JsonPropertyName("right_module")]
    public string RightModule { get; set; } = "";

    [JsonPropertyName("is_clone")]
    public bool IsClone { get; set; }
}

public sealed class ControllerRow
{
    public string Name { get; init; } = "DualSense";
    public string Address { get; init; } = "";
    public bool IsUsb { get; init; }
    public bool IsVirtual { get; init; }
    public bool Online { get; init; }
    public bool Registered { get; init; }
    public bool Connected { get; init; }
    public string Endpoint { get; init; } = "";
    public string Profile { get; init; } = "";
    public string Serial { get; set; } = "";
    public string BuildTime { get; set; } = "";
    public string Firmware { get; set; } = "";
    public string Board { get; set; } = "";
    public string ColorName { get; set; } = "";
    public string MacAddress { get; set; } = "";
    public string ConnectionDisplay =>
        IsVirtual ? "虚拟有线（蓝牙桥接）" : IsUsb ? "USB 连接" : $"蓝牙 {(Online ? "在线" : "离线")}";
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
