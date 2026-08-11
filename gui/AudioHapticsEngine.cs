using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading.Channels;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace VdsGui;

public sealed class AudioHapticsEngine : IAsyncDisposable
{
    private const string PipeName = "vdsd-audio";
    private const int HapticsSampleRate = 48_000;
    private const int FramesPerChunk = 512;
    // Stream protocol v2: full 4-channel USB audio layout per frame -
    // [speaker L, speaker R, haptics L, haptics R]. The speaker pair feeds
    // the controller speaker / headphone jack (Opus-encoded by the daemon),
    // the haptics pair drives the voice coils.
    private const int ChannelsPerFrame = 4;
    private const int BytesPerChunk = FramesPerChunk * ChannelsPerFrame * sizeof(short);
    // This property contains the underlying PnP instance path (for example
    // {1}.USB\VID_054C&PID_0CE6&MI_00\...). MMDevice.InstanceId returns
    // "Unknown" for these USB audio endpoints, so read ControllerDeviceId.
    private static readonly PropertyKey ControllerDeviceIdKey =
        PropertyKeys.PKEY_Device_ControllerDeviceId;
    private readonly object _lifecycleLock = new();
    private readonly HapticsProcessor _processor = new();
    private MMDeviceEnumerator? _enumerator;
    private MMDevice? _device;
    private WasapiLoopbackCapture? _capture;
    private NamedPipeClientStream? _pipe;
    private MMDevice? _usbAudioDevice;
    private WasapiOut? _usbAudioOut;
    private BufferedWaveProvider? _usbAudioBuffer;
    private IntPtr _ds4Device;
    private IntPtr _dsHidDevice;
    private MMDevice? _ds4SpeakerDevice;
    private BufferedWaveProvider? _ds4SpeakerBuffer;
    private WasapiOut? _ds4SpeakerOut;
    private double _ds4SpeakerAccumulator;
    private IntPtr _ds4BtDevice;
    private double _btResamplePos;
    private short _btPrevMono;
    private ushort _btFrameNumber;
    private readonly List<byte[]> _btSbcFrames = new(4);
    private Channel<byte[]>? _chunks;
    private CancellationTokenSource? _cancellation;
    private Task? _writerTask;
    private AudioHapticsSettings _settings = new();
    private long _lastLevelsTimestamp;
    // DS4 0x05 output report volume bytes [19]/[20]/[22]. The DS4 firmware
    // applies these every time a report arrives, so every rumble report must
    // carry them or the headset output silently mutes.
    private byte _ds4SpeakerVolume;
    private bool _ds4SpeakerConfigured;
    private bool _running;

    public bool IsRunning
    {
        get
        {
            lock (_lifecycleLock)
            {
                return _running;
            }
        }
    }

    public event EventHandler<AudioHapticsLevels>? LevelsChanged;
    public event EventHandler<string>? Faulted;

    public static IReadOnlyList<AudioRenderDevice> GetRenderDevices()
    {
        using var enumerator = new MMDeviceEnumerator();
        string? defaultId = null;
        try
        {
            using var defaultDevice =
                enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            defaultId = defaultDevice.ID;
        }
        catch
        {
        }

        var devices = new List<AudioRenderDevice>();
        foreach (var device in enumerator.EnumerateAudioEndPoints(
                     DataFlow.Render, DeviceState.Active))
        {
            using (device)
            {
                // Controller endpoints are intentionally listed too: users
                // may want to drive haptics from the audio already routed to
                // the controller. The feedback guard lives in StartAsync,
                // which stops the speaker write-back when capture and target
                // are the same controller.
                devices.Add(new AudioRenderDevice(
                    device.ID,
                    device.FriendlyName,
                    string.Equals(
                        device.ID, defaultId, StringComparison.OrdinalIgnoreCase),
                    GetControllerDeviceId(device) ?? ""));
            }
        }
        return devices
            .OrderByDescending(device => device.IsDefault)
            .ThenBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase)
            .ToArray();
    }

    public void UpdateSettings(AudioHapticsSettings settings)
    {
        Volatile.Write(ref _settings, settings.Clone());
    }

    public async Task StartAsync(AudioHapticsSettings settings, CancellationToken cancellationToken = default)
    {
        lock (_lifecycleLock)
        {
            if (_running)
            {
                return;
            }
            _running = true;
        }

        try
        {
            _enumerator = new MMDeviceEnumerator();
            _device = ResolveDevice(_enumerator, settings.DeviceId);
            settings = ApplyFeedbackGuard(_device, settings);
            UpdateSettings(settings);
            _capture = new WasapiLoopbackCapture(_device);
            SetCaptureBufferMilliseconds(
                _capture, Math.Clamp((int)settings.CaptureLatencyMs, 10, 300));
            _capture.DataAvailable += Capture_DataAvailable;
            _capture.RecordingStopped += Capture_RecordingStopped;
            _processor.Reset(_capture.WaveFormat);
            _chunks = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(20)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = true
            });
            _cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            if (settings.TargetMode == "usb_audio")
            {
                (_usbAudioDevice, _usbAudioBuffer, _usbAudioOut) =
                    OpenUsbAudioTarget(
                        settings.TargetDeviceId,
                        Math.Clamp((int)settings.UsbLatencyMs, 30, 300),
                        Math.Clamp((int)settings.PlaybackQueueMs, 20, 500));
                if (settings.SpeakerEnabled)
                {
                    // The DualSense firmware routes audio to the headphone
                    // jack by default (even with no headset attached) and
                    // keeps the internal speaker volume near zero, so the
                    // speaker L/R channels written to the USB audio endpoint
                    // stay silent unless we explicitly route them to the
                    // internal speaker via a HID output report.
                    _dsHidDevice = OpenDs4HidTarget(settings.TargetDeviceId);
                    ConfigureDualSenseSpeaker(
                        _dsHidDevice,
                        Math.Clamp(settings.SpeakerPreamp, 0, 7));
                }
                _usbAudioOut.Play();
                _writerTask = WriteUsbAudioChunksAsync(
                    _usbAudioBuffer, _chunks.Reader, _cancellation.Token);
            }
            else if (settings.TargetMode == "usb_rumble")
            {
                _ds4Device = OpenDs4HidTarget(settings.TargetDeviceId);
                if (settings.SpeakerEnabled)
                {
                    // The DS4 firmware re-applies the 0x05 volume bytes
                    // whenever an output report arrives, and zeroed bytes mute
                    // the headset amplifier even for native Windows playback.
                    // When forwarding is on, set the slider volume up front so
                    // the speaker path is active from the first chunk.
                    ConfigureDs4Speaker(
                        _ds4Device,
                        Math.Clamp((int)settings.SpeakerVolumePercent, 0, 100));
                    (_ds4SpeakerDevice, _ds4SpeakerBuffer, _ds4SpeakerOut) =
                        OpenDs4SpeakerTarget(
                            settings.TargetDeviceId,
                            Math.Clamp((int)settings.UsbLatencyMs, 30, 300),
                            Math.Clamp((int)settings.PlaybackQueueMs, 20, 500));
                    _ds4SpeakerOut.Play();
                }
                _writerTask = WriteDs4RumbleChunksAsync(
                    _ds4Device,
                    _ds4SpeakerBuffer,
                    _chunks.Reader,
                    _cancellation.Token);
            }
            else if (settings.TargetMode == "bt_sbc")
            {
                // DualShock 4 speaker audio over Bluetooth: there is no
                // Windows audio endpoint for the DS4 on BT, so the captured
                // stream is SBC-encoded here and delivered through HID
                // reports (one-shot 0x11 control, then 0x17 audio batches).
                _ds4BtDevice = OpenDs4HidTarget(settings.TargetDeviceId);
                ConfigureDs4BtSpeaker(
                    _ds4BtDevice,
                    Math.Clamp((int)settings.SpeakerVolumePercent, 0, 100));
                _writerTask = WriteDs4BtAudioChunksAsync(
                    _ds4BtDevice, _chunks.Reader, _cancellation.Token);
            }
            else
            {
                _pipe = new NamedPipeClientStream(
                    ".",
                    PipeName,
                    PipeDirection.Out,
                    PipeOptions.Asynchronous | PipeOptions.WriteThrough);
                await _pipe.ConnectAsync(2_500, _cancellation.Token);
                await WriteHeaderAsync(_pipe, _cancellation.Token);
                _writerTask = WriteChunksAsync(
                    _pipe, _chunks.Reader, _cancellation.Token);
            }
            _capture.StartRecording();
        }
        catch
        {
            await StopCoreAsync(false);
            throw;
        }
    }

    public async Task StopAsync() => await StopCoreAsync(true);

    public async ValueTask DisposeAsync() => await StopCoreAsync(false);

    private static MMDevice ResolveDevice(MMDeviceEnumerator enumerator, string id)
    {
        if (!string.IsNullOrWhiteSpace(id))
        {
            MMDevice selected;
            try
            {
                selected = enumerator.GetDevice(id);
            }
            catch (Exception error)
            {
                throw new InvalidOperationException(
                    "所选桌面音频触觉播放设备当前不可用，请刷新后重新选择。", error);
            }

            var keepSelected = false;
            try
            {
                if (selected.State != DeviceState.Active)
                {
                    throw new InvalidOperationException(
                        "所选桌面音频触觉播放设备当前未启用，请刷新后重新选择。");
                }
                keepSelected = true;
                return selected;
            }
            finally
            {
                if (!keepSelected)
                {
                    selected.Dispose();
                }
            }
        }

        MMDevice? defaultDevice = null;
        try
        {
            defaultDevice = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            return defaultDevice;
        }
        catch
        {
        }
        finally
        {
            defaultDevice?.Dispose();
        }

        throw new InvalidOperationException(
            "没有可用的桌面音频触觉播放设备。");
    }

    /// <summary>
    /// When the loopback source is the same controller that the speaker path
    /// writes to, the written audio would be captured again and howl. Haptics
    /// stay enabled; only the speaker write-back is dropped.
    /// </summary>
    private static AudioHapticsSettings ApplyFeedbackGuard(
        MMDevice captureDevice, AudioHapticsSettings settings)
    {
        if (!settings.SpeakerEnabled || string.IsNullOrWhiteSpace(settings.TargetDeviceId))
        {
            return settings;
        }
        var (vid, pid) = ParseVidPid(settings.TargetDeviceId);
        if (string.IsNullOrEmpty(vid) || string.IsNullOrEmpty(pid))
        {
            return settings;
        }
        var controllerPath = GetControllerDeviceId(captureDevice);
        if (string.IsNullOrWhiteSpace(controllerPath) ||
            !controllerPath.Contains(
                $"VID_{vid}&PID_{pid}", StringComparison.OrdinalIgnoreCase))
        {
            return settings;
        }
        var guarded = settings.Clone();
        guarded.SpeakerEnabled = false;
        return guarded;
    }

    public static bool IsControllerAudioEndpoint(string deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            return false;
        }
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            using var device = enumerator.GetDevice(deviceId);
            return IsDualSenseAudioEndpoint(device);
        }
        catch
        {
            return false;
        }
    }

    private static bool IsDualSenseAudioEndpoint(MMDevice device)
    {
        string instancePath = "";
        try
        {
            if (device.Properties.Contains(ControllerDeviceIdKey))
            {
                instancePath =
                    device.Properties[ControllerDeviceIdKey].Value?.ToString() ?? "";
            }
        }
        catch
        {
        }

        if (instancePath.Contains(
                @"USB\VID_054C&PID_0CE6", StringComparison.OrdinalIgnoreCase) ||
            instancePath.Contains(
                @"USB\VID_054C&PID_0DF2", StringComparison.OrdinalIgnoreCase) ||
            instancePath.Contains(
                @"USB\VID_054C&PID_05C4", StringComparison.OrdinalIgnoreCase) ||
            instancePath.Contains(
                @"USB\VID_054C&PID_09CC", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        // Product strings are stable USB descriptor values and provide a
        // fallback for systems that do not expose the PnP property above.
        var name = $"{device.FriendlyName} {device.DeviceFriendlyName}";
        return name.Contains("DualSense Wireless Controller", StringComparison.OrdinalIgnoreCase) ||
               name.Contains("DualSense Edge Wireless Controller", StringComparison.OrdinalIgnoreCase) ||
               name.Contains("DUALSHOCK 4", StringComparison.OrdinalIgnoreCase) ||
               name.Contains("DualShock 4", StringComparison.OrdinalIgnoreCase);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_SetOutputReport(
        IntPtr hidDeviceObject,
        byte[] reportBuffer,
        uint reportBufferLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(
        IntPtr hFile,
        byte[] lpBuffer,
        uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten,
        IntPtr lpOverlapped);

    [DllImport("hid.dll")]
    private static extern void HidD_GetHidGuid(out Guid hidGuid);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr SetupDiGetClassDevsA(
        ref Guid classGuid, string? enumerator, IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid classGuid,
        uint memberIndex, ref SpDeviceInterfaceData deviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetailA(
        IntPtr deviceInfoSet, ref SpDeviceInterfaceData deviceInterfaceData,
        IntPtr detail, uint detailSize, out uint requiredSize,
        ref SpDevInfoData deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    private const uint DigcfPresent = 0x00000002;
    private const uint DigcfDeviceInterface = 0x00000010;

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDeviceInterfaceData
    {
        public uint CbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDevInfoData
    {
        public uint CbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    private struct SpDeviceInterfaceDetailDataA
    {
        public uint CbSize;
        public byte DevicePath;
    }

    private static string? GetControllerDeviceId(MMDevice device)
    {
        try
        {
            return device.Properties.Contains(ControllerDeviceIdKey)
                ? device.Properties[ControllerDeviceIdKey].Value?.ToString()
                : null;
        }
        catch
        {
            return null;
        }
    }

    private static (string Vid, string Pid) ParseVidPid(string hidPath)
    {
        var match = Regex.Match(
            hidPath,
            @"vid_([0-9a-f]{4})&pid_([0-9a-f]{4})",
            RegexOptions.IgnoreCase);
        return match.Success
            ? (match.Groups[1].Value, match.Groups[2].Value)
            : ("", "");
    }

    private static void SetCaptureBufferMilliseconds(
        WasapiLoopbackCapture capture, int milliseconds)
    {
        // NAudio's WasapiLoopbackCapture does not expose the capture buffer
        // length in its public constructor, but the base WasapiCapture reads
        // this private field when the client initializes on StartRecording.
        // NAudio is shipped with the app, so the field name is stable.
        try
        {
            var field = typeof(WasapiCapture).GetField(
                "audioBufferMillisecondsLength",
                BindingFlags.NonPublic | BindingFlags.Instance);
            field?.SetValue(capture, milliseconds);
        }
        catch
        {
            // Keep the NAudio default when the field is unavailable.
        }
    }

    private static bool DeviceMatchesVidPid(
        MMDevice device, string vid, string pid)
    {
        var controllerPath = GetControllerDeviceId(device);
        return !string.IsNullOrWhiteSpace(controllerPath) &&
               controllerPath.Contains(
                   $"VID_{vid}&PID_{pid}", StringComparison.OrdinalIgnoreCase);
    }

    private static (MMDevice Device, BufferedWaveProvider Buffer, WasapiOut Output)
        OpenUsbAudioTarget(string hidPath, int latencyMs, int playbackQueueMs)
    {
        var (vid, pid) = ParseVidPid(hidPath);
        if (string.IsNullOrEmpty(vid) || string.IsNullOrEmpty(pid))
        {
            throw new InvalidOperationException("USB 手柄路径缺少 VID/PID，无法匹配音频端点。");
        }

        using var enumerator = new MMDeviceEnumerator();
        foreach (var device in enumerator.EnumerateAudioEndPoints(
                     DataFlow.Render, DeviceState.Active))
        {
            var keep = false;
            try
            {
                if (!DeviceMatchesVidPid(device, vid, pid))
                {
                    continue;
                }
                var mix = device.AudioClient.MixFormat;
                if (mix.Channels < 4)
                {
                    throw new InvalidOperationException(
                        $"USB DualSense 音频端点只有 {mix.Channels} 声道，不支持音圈触觉。");
                }
                var format = mix as WaveFormatExtensible
                    ?? new WaveFormatExtensible(
                        mix.SampleRate, mix.BitsPerSample, mix.Channels);
                var buffer = new BufferedWaveProvider(format)
                {
                    DiscardOnBufferOverflow = true,
                    // User-adjustable queue that lets the loopback capture
                    // clock and the controller's USB audio clock drift
                    // without starving the endpoint into clicks/pops.
                    BufferDuration = TimeSpan.FromMilliseconds(playbackQueueMs)
                };
                var output = new WasapiOut(
                    device,
                    AudioClientShareMode.Shared,
                    true,
                    latencyMs);
                output.Init(buffer);
                keep = true;
                return (device, buffer, output);
            }
            finally
            {
                if (!keep)
                {
                    device.Dispose();
                }
            }
        }
        throw new InvalidOperationException(
            "未找到与所选 USB DualSense 对应的音频端点，请确认手柄已开启音频。");
    }

    /// <summary>
    /// Locates the Bluetooth HID interface of a DualShock 4. Bluetooth HID
    /// paths embed the HID-over-Bluetooth service GUID
    /// {00001124-0000-1000-8000-00805f9b34fb}, which distinguishes them from
    /// the wired USB HID interface.
    /// </summary>
    internal static string? FindDs4BtHidPath()
    {
        Guid hidGuid;
        HidD_GetHidGuid(out hidGuid);
        var set = SetupDiGetClassDevsA(
            ref hidGuid, null, IntPtr.Zero, DigcfPresent | DigcfDeviceInterface);
        if (set == IntPtr.Zero || set == new IntPtr(-1))
        {
            return null;
        }
        try
        {
            for (uint index = 0; ; index++)
            {
                var ifData = new SpDeviceInterfaceData
                {
                    CbSize = (uint)Marshal.SizeOf<SpDeviceInterfaceData>()
                };
                if (!SetupDiEnumDeviceInterfaces(
                        set, IntPtr.Zero, ref hidGuid, index, ref ifData))
                {
                    break;
                }
                uint required = 0;
                var unused = new SpDevInfoData
                {
                    CbSize = (uint)Marshal.SizeOf<SpDevInfoData>()
                };
                SetupDiGetDeviceInterfaceDetailA(
                    set, ref ifData, IntPtr.Zero, 0, out required, ref unused);
                if (required == 0)
                {
                    continue;
                }
                IntPtr detail = Marshal.AllocHGlobal((int)required);
                try
                {
                    Marshal.WriteInt32(
                        detail, Marshal.SizeOf<SpDeviceInterfaceDetailDataA>());
                    var info = new SpDevInfoData
                    {
                        CbSize = (uint)Marshal.SizeOf<SpDevInfoData>()
                    };
                    if (!SetupDiGetDeviceInterfaceDetailA(
                            set, ref ifData, detail, required, out _, ref info))
                    {
                        continue;
                    }
                    string? path = Marshal.PtrToStringAnsi(
                        IntPtr.Add(detail, (int)Marshal.OffsetOf<
                            SpDeviceInterfaceDetailDataA>("DevicePath")));
                    if (path is not null &&
                        path.Contains(
                            "pid&09cc",
                            StringComparison.OrdinalIgnoreCase) &&
                        path.Contains(
                            "{00001124-0000-1000-8000-00805f9b34fb}",
                            StringComparison.OrdinalIgnoreCase))
                    {
                        return path;
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(detail);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return null;
    }

    private static IntPtr OpenDs4HidTarget(string hidPath)
    {
        const uint genericRead = 0x80000000;
        const uint genericWrite = 0x40000000;
        const uint shareReadWrite = 0x00000003;
        const uint openExisting = 3;
        var handle = CreateFileW(
            hidPath,
            genericRead | genericWrite,
            shareReadWrite,
            IntPtr.Zero,
            openExisting,
            0,
            IntPtr.Zero);
        if (handle == IntPtr.Zero || handle == new IntPtr(-1))
        {
            throw new InvalidOperationException(
                $"无法打开 USB 手柄 HID 设备：{(uint)Marshal.GetLastWin32Error():X8}");
        }
        return handle;
    }

    private static (MMDevice Device, BufferedWaveProvider Buffer, WasapiOut Output)
        OpenDs4SpeakerTarget(string hidPath, int latencyMs, int playbackQueueMs)
    {
        var (vid, pid) = ParseVidPid(hidPath);
        if (string.IsNullOrEmpty(vid) || string.IsNullOrEmpty(pid))
        {
            throw new InvalidOperationException("USB 手柄路径缺少 VID/PID，无法匹配扬声器端点。");
        }

        using var enumerator = new MMDeviceEnumerator();
        foreach (var device in enumerator.EnumerateAudioEndPoints(
                     DataFlow.Render, DeviceState.Active))
        {
            var keep = false;
            try
            {
                if (!DeviceMatchesVidPid(device, vid, pid))
                {
                    continue;
                }
                var mix = device.AudioClient.MixFormat;
                if (mix.Channels < 1)
                {
                    continue;
                }
                var format = mix as WaveFormatExtensible
                    ?? new WaveFormatExtensible(
                        mix.SampleRate, mix.BitsPerSample, mix.Channels);
                var buffer = new BufferedWaveProvider(format)
                {
                    DiscardOnBufferOverflow = true,
                    BufferDuration = TimeSpan.FromMilliseconds(playbackQueueMs)
                };
                var output = new WasapiOut(
                    device,
                    AudioClientShareMode.Shared,
                    false,
                    latencyMs);
                output.Init(buffer);
                keep = true;
                return (device, buffer, output);
            }
            finally
            {
                if (!keep)
                {
                    device.Dispose();
                }
            }
        }
        throw new InvalidOperationException(
            "未找到与所选 USB DS4 对应的扬声器音频端点。");
    }

    private static void WriteDs4Report(IntPtr device, byte[] report)
    {
        if (WriteFile(device, report, (uint)report.Length, out _, IntPtr.Zero))
        {
            return;
        }
        HidD_SetOutputReport(device, report, (uint)report.Length);
    }

    private void ConfigureDs4Speaker(IntPtr device, int volumePercent)
    {
        // DS4 USB main output report, ID 0x05 (32 bytes). Layout:
        //   [1]  valid flags (0x01 rumble, 0x02 LED, 0x04 LED blink)
        //   [4]  rumble right, [5] rumble left
        //   [6..8]  LED RGB, [9..10] LED blink on/off
        //   [11..18] reserved
        //   [19] volume left, [20] volume right, [21] volume mic,
        //   [22] volume speaker
        // Volume range seen in real PS4 captures is 0x00-0x7F.
        var report = new byte[32];
        report[0] = 0x05;
        report[1] = 0x01; // motor only; leave LED state untouched
        report[4] = 0;
        report[5] = 0;
        _ds4SpeakerVolume = (byte)Math.Clamp(volumePercent * 0x7F / 100, 0, 0x7F);
        report[19] = _ds4SpeakerVolume;
        report[20] = _ds4SpeakerVolume;
        report[22] = _ds4SpeakerVolume;
        _ds4SpeakerConfigured = true;
        WriteDs4Report(device, report);
    }

    private static void ConfigureDs4BtSpeaker(IntPtr device, int volumePercent)
    {
        // One-shot 0x11 control report: arms the DS4 Bluetooth audio plane
        // and sets headphone + speaker volume. The DS4 has no per-tick volume
        // byte over Bluetooth; the volumes live in this one-shot report.
        var report = Ds4BtAudioProtocol.BuildControlReport(
            speakerEnabled: true,
            volumePercent: volumePercent,
            bluetoothPollRate: 4);
        WriteDs4Report(device, report);
    }

    private void SendDs4Rumble(IntPtr device, byte right, byte left)
    {
        var report = new byte[32];
        report[0] = 0x05;
        report[1] = 0x01; // motor only; 0x02 LED, 0x04 flash
        report[4] = right;
        report[5] = left;
        if (_ds4SpeakerVolume > 0)
        {
            // Keep the headset volume applied on every report; the DS4
            // firmware re-applies these bytes whenever a 0x05 report arrives.
            report[19] = _ds4SpeakerVolume;
            report[20] = _ds4SpeakerVolume;
            report[22] = _ds4SpeakerVolume;
        }
        WriteDs4Report(device, report);
    }

    private static void ConfigureDualSenseSpeaker(IntPtr device, int preamp)
    {
        // DualSense USB main output report, ID 0x02. Windows reports the
        // output report as 48 bytes including the ID byte (1 + 47 common).
        var report = new byte[48];
        report[0] = 0x02;  // report ID
        report[1] = 0xA0;  // valid_flag0: AUDIO_CONTROL_ENABLE | SPEAKER_VOLUME_ENABLE
        report[2] = 0x80;  // valid_flag1: AUDIO_CONTROL2_ENABLE
        // Speaker volume, usable range 0x3d-0x64. Use 100% as plain unity
        // gain (no preamp boost below) so the software volume slider has the
        // full usable range; the earlier crackle came from the +6dB preamp
        // pushing a full-scale desktop mix past the DAC headroom.
        report[6] = 0x64;  // speaker volume (100%)
        report[8] = 0x30;  // audio_control: output path = 0b11, route R channel to speaker
        report[38] = (byte)preamp; // audio_control2: speaker preamp gain 0-7
        WriteFile(device, report, (uint)report.Length, out _, IntPtr.Zero);
    }

    private static byte[] ConvertChunkToFloat(byte[] chunk)
    {
        var frames = chunk.Length / (ChannelsPerFrame * sizeof(short));
        var output = new byte[frames * ChannelsPerFrame * sizeof(float)];
        var destination = 0;
        for (var sample = 0; sample < frames * ChannelsPerFrame; ++sample)
        {
            var source = sample * sizeof(short);
            var value = BinaryPrimitives.ReadInt16LittleEndian(chunk.AsSpan(source, 2));
            var sampleFloat = value / 32768f;
            BinaryPrimitives.WriteSingleLittleEndian(
                output.AsSpan(destination, 4), sampleFloat);
            destination += sizeof(float);
        }
        return output;
    }

    private async Task WriteUsbAudioChunksAsync(
        BufferedWaveProvider buffer,
        ChannelReader<byte[]> reader,
        CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var chunk in reader.ReadAllAsync(cancellationToken))
            {
                var floatChunk = ConvertChunkToFloat(chunk);
                buffer.AddSamples(floatChunk, 0, floatChunk.Length);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"USB 音频触觉输出中断：{error.Message}");
        }
    }

    private async Task WriteDs4RumbleChunksAsync(
        IntPtr device,
        BufferedWaveProvider? speakerBuffer,
        ChannelReader<byte[]> reader,
        CancellationToken cancellationToken)
    {
        var previousRight = 0;
        var previousLeft = 0;
        try
        {
            await foreach (var chunk in reader.ReadAllAsync(cancellationToken))
            {
                var frames = chunk.Length / (ChannelsPerFrame * sizeof(short));
                var rightPeak = 0;
                var leftPeak = 0;
                for (var frame = 0; frame < frames; ++frame)
                {
                    var baseIndex = frame * ChannelsPerFrame * sizeof(short);
                    var hapticsLeft = Math.Abs((int)BinaryPrimitives.ReadInt16LittleEndian(
                        chunk.AsSpan(baseIndex + 2 * sizeof(short), 2)));
                    var hapticsRight = Math.Abs((int)BinaryPrimitives.ReadInt16LittleEndian(
                        chunk.AsSpan(baseIndex + 3 * sizeof(short), 2)));
                    leftPeak = Math.Max(leftPeak, hapticsLeft);
                    rightPeak = Math.Max(rightPeak, hapticsRight);
                }
                var targetRight = Math.Clamp(rightPeak * 255 / 32768, 0, 255);
                var targetLeft = Math.Clamp(leftPeak * 255 / 32768, 0, 255);
                previousRight = SmoothMotor(previousRight, targetRight);
                previousLeft = SmoothMotor(previousLeft, targetLeft);
                if (!_ds4SpeakerConfigured)
                {
                    // Forwarding-off mode: do not touch the DS4 volume until
                    // the first rumble report is actually needed, then set it
                    // to full so the report bytes never mute the headset amp.
                    ConfigureDs4Speaker(device, 100);
                }
                SendDs4Rumble(device, (byte)previousRight, (byte)previousLeft);
                if (speakerBuffer is not null)
                {
                    var speakerChunk = ResampleDs4Speaker(
                        chunk, speakerBuffer.WaveFormat, ref _ds4SpeakerAccumulator);
                    if (speakerChunk.Length > 0)
                    {
                        speakerBuffer.AddSamples(
                            speakerChunk, 0, speakerChunk.Length);
                    }
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"DS4 马达输出中断：{error.Message}");
        }
    }

    private static byte[] ResampleDs4Speaker(
        byte[] chunk, WaveFormat format, ref double accumulator)
    {
        var inputFrames = chunk.Length / (ChannelsPerFrame * sizeof(short));
        var ratio = format.SampleRate / (double)HapticsSampleRate;
        accumulator += inputFrames * ratio;
        var outputFrames = (int)accumulator;
        accumulator -= outputFrames;
        if (outputFrames <= 0)
        {
            return Array.Empty<byte>();
        }

        var output = new byte[outputFrames * Math.Max(1, format.Channels) * sizeof(float)];
        var destination = 0;
        for (var frame = 0; frame < outputFrames; ++frame)
        {
            var source = frame / ratio;
            var index0 = (int)source;
            var fraction = (float)(source - index0);
            var index1 = Math.Min(index0 + 1, inputFrames - 1);
            var base0 = index0 * ChannelsPerFrame * sizeof(short);
            var base1 = index1 * ChannelsPerFrame * sizeof(short);
            var left0 = BinaryPrimitives.ReadInt16LittleEndian(chunk.AsSpan(base0, 2));
            var right0 = BinaryPrimitives.ReadInt16LittleEndian(chunk.AsSpan(base0 + 2, 2));
            var left1 = BinaryPrimitives.ReadInt16LittleEndian(chunk.AsSpan(base1, 2));
            var right1 = BinaryPrimitives.ReadInt16LittleEndian(chunk.AsSpan(base1 + 2, 2));
            var mono = (left0 + (left1 - left0) * fraction) +
                       (right0 + (right1 - right0) * fraction);
            var sampleFloat = mono * 0.5f / 32768f;
            for (var channel = 0; channel < format.Channels; ++channel)
            {
                BinaryPrimitives.WriteSingleLittleEndian(
                    output.AsSpan(destination, 4), sampleFloat);
                destination += sizeof(float);
            }
        }
        return output;
    }

    private async Task WriteDs4BtAudioChunksAsync(
        IntPtr device,
        ChannelReader<byte[]> reader,
        CancellationToken cancellationToken)
    {
        var encoder = new SBC.DualShock4SbcEncoder();
        var left = new short[SBC.DualShock4SbcEncoder.SamplesPerChannel];
        var right = new short[SBC.DualShock4SbcEncoder.SamplesPerChannel];
        var frameBytes = new byte[SBC.DualShock4SbcEncoder.FrameLength];
        var pending = new List<short>(SBC.DualShock4SbcEncoder.SamplesPerChannel * 2);
        _btSbcFrames.Clear();
        _btFrameNumber = 0;
        _btResamplePos = 0;
        _btPrevMono = 0;
        try
        {
            await foreach (var chunk in reader.ReadAllAsync(cancellationToken))
            {
                var inputFrames = chunk.Length / (ChannelsPerFrame * sizeof(short));
                for (var index = 0; index < inputFrames; index++)
                {
                    var baseIndex = index * ChannelsPerFrame * sizeof(short);
                    var leftSample = BinaryPrimitives.ReadInt16LittleEndian(
                        chunk.AsSpan(baseIndex, 2));
                    var rightSample = BinaryPrimitives.ReadInt16LittleEndian(
                        chunk.AsSpan(baseIndex + 2, 2));
                    var mono = (short)((leftSample + rightSample) / 2);
                    // Linear resample 48 kHz -> 32 kHz. Output samples sit at
                    // source positions 0, 1.5, 3.0, ... (48k/32k).
                    while (_btResamplePos <= index)
                    {
                        var fraction = _btResamplePos - (index - 1);
                        pending.Add((short)(
                            _btPrevMono + (mono - _btPrevMono) * fraction));
                        _btResamplePos +=
                            Ds4BtAudioProtocol.SampleRate / (double)HapticsSampleRate;
                        if (pending.Count >= SBC.DualShock4SbcEncoder.SamplesPerChannel)
                        {
                            EncodeAndSendDs4BtBlock(
                                device, encoder, pending, left, right, frameBytes);
                        }
                    }
                    _btPrevMono = mono;
                }
            }
            // Drain the tail so the last block is not dropped on stop.
            while (pending.Count >= SBC.DualShock4SbcEncoder.SamplesPerChannel)
            {
                EncodeAndSendDs4BtBlock(
                    device, encoder, pending, left, right, frameBytes);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"DS4 蓝牙扬声器输出中断：{error.Message}");
        }
    }

    private void EncodeAndSendDs4BtBlock(
        IntPtr device,
        SBC.DualShock4SbcEncoder encoder,
        List<short> pending,
        short[] left,
        short[] right,
        byte[] frameBytes)
    {
        for (var sample = 0; sample < SBC.DualShock4SbcEncoder.SamplesPerChannel; sample++)
        {
            left[sample] = pending[sample];
            right[sample] = pending[sample];
        }
        pending.RemoveRange(0, SBC.DualShock4SbcEncoder.SamplesPerChannel);
        encoder.Encode(left, right, frameBytes);
        _btSbcFrames.Add((byte[])frameBytes.Clone());
        if (_btSbcFrames.Count == 4)
        {
            var report = Ds4BtAudioProtocol.BuildSpeakerReport(
                _btFrameNumber, _btSbcFrames.ToArray());
            _btFrameNumber++;
            _btSbcFrames.Clear();
            WriteDs4Report(device, report);
        }
    }

    private static int SmoothMotor(int previous, int target)
    {
        if (target > previous)
        {
            // Fast attack (~6 ms per chunk at 512 frames / 48 kHz).
            return previous + (target - previous) * 3 / 4;
        }
        // Slower release to avoid harsh motor clicks.
        return previous + (target - previous) / 3;
    }

    private async Task StopCoreAsync(bool sendSilence)
    {
        WasapiLoopbackCapture? capture;
        Channel<byte[]>? chunks;
        Task? writerTask;
        CancellationTokenSource? cancellation;
        NamedPipeClientStream? pipe;
        WasapiOut? usbAudioOut;
        MMDevice? usbAudioDevice;
        IntPtr ds4Device;
        IntPtr ds4BtDevice;
        IntPtr dsHidDevice;
        WasapiOut? ds4SpeakerOut;
        MMDevice? ds4SpeakerDevice;
        lock (_lifecycleLock)
        {
            if (!_running && _capture is null && _pipe is null &&
                _usbAudioOut is null && _ds4Device == IntPtr.Zero &&
                _dsHidDevice == IntPtr.Zero &&
                _ds4SpeakerOut is null)
            {
                return;
            }
            _running = false;
            capture = _capture;
            chunks = _chunks;
            writerTask = _writerTask;
            cancellation = _cancellation;
            pipe = _pipe;
            usbAudioOut = _usbAudioOut;
                usbAudioDevice = _usbAudioDevice;
                ds4Device = _ds4Device;
                ds4BtDevice = _ds4BtDevice;
                dsHidDevice = _dsHidDevice;
                ds4SpeakerOut = _ds4SpeakerOut;
                ds4SpeakerDevice = _ds4SpeakerDevice;
            _capture = null;
            _chunks = null;
            _writerTask = null;
            _cancellation = null;
            _pipe = null;
            _usbAudioOut = null;
                _usbAudioDevice = null;
                _ds4Device = IntPtr.Zero;
                _ds4BtDevice = IntPtr.Zero;
                _dsHidDevice = IntPtr.Zero;
            _ds4SpeakerOut = null;
            _ds4SpeakerDevice = null;
        }

        if (capture is not null)
        {
            capture.DataAvailable -= Capture_DataAvailable;
            capture.RecordingStopped -= Capture_RecordingStopped;
            try
            {
                capture.StopRecording();
            }
            catch
            {
            }
            capture.Dispose();
        }

        if (chunks is not null)
        {
            if (sendSilence)
            {
                chunks.Writer.TryWrite(new byte[BytesPerChunk]);
                chunks.Writer.TryWrite(new byte[BytesPerChunk]);
            }
            chunks.Writer.TryComplete();
        }
        if (writerTask is not null)
        {
            try
            {
                await writerTask;
            }
            catch
            {
            }
        }
        cancellation?.Cancel();
        cancellation?.Dispose();
        pipe?.Dispose();
        if (usbAudioOut is not null)
        {
            try
            {
                usbAudioOut.Stop();
            }
            catch
            {
            }
            usbAudioOut.Dispose();
        }
        usbAudioDevice?.Dispose();
        if (ds4Device != IntPtr.Zero)
        {
            SendDs4Rumble(ds4Device, 0, 0);
            CloseHandle(ds4Device);
        }
        if (ds4BtDevice != IntPtr.Zero)
        {
            try
            {
                // Disarm the Bluetooth audio plane on stop so the DS4 does
                // not stay in audio mode after playback ends.
                var disarm = Ds4BtAudioProtocol.BuildControlReport(
                    speakerEnabled: false,
                    volumePercent: 0,
                    bluetoothPollRate: 4);
                WriteDs4Report(ds4BtDevice, disarm);
            }
            catch
            {
            }
            CloseHandle(ds4BtDevice);
        }
        if (dsHidDevice != IntPtr.Zero)
        {
            CloseHandle(dsHidDevice);
        }
        if (ds4SpeakerOut is not null)
        {
            try
            {
                ds4SpeakerOut.Stop();
            }
            catch
            {
            }
            ds4SpeakerOut.Dispose();
        }
        ds4SpeakerDevice?.Dispose();
        _device?.Dispose();
        _device = null;
        _enumerator?.Dispose();
        _enumerator = null;
    }

    private async Task WriteChunksAsync(
        Stream stream,
        ChannelReader<byte[]> reader,
        CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var chunk in reader.ReadAllAsync(cancellationToken))
            {
                await stream.WriteAsync(chunk, cancellationToken);
            }
            await stream.FlushAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"音频触觉连接中断：{error.Message}");
        }
    }

    private static async Task WriteHeaderAsync(Stream stream, CancellationToken cancellationToken)
    {
        var header = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0, 4), 0x41534456);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(4, 2), 2);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(6, 2), ChannelsPerFrame);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8, 4), HapticsSampleRate);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12, 4), FramesPerChunk);
        await stream.WriteAsync(header, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    private void Capture_DataAvailable(object? sender, WaveInEventArgs eventArgs)
    {
        try
        {
            var capture = _capture;
            var writer = _chunks?.Writer;
            if (capture is null || writer is null)
            {
                return;
            }
            var settings = Volatile.Read(ref _settings);
            var result = _processor.Process(eventArgs.Buffer, eventArgs.BytesRecorded, capture.WaveFormat, settings);
            foreach (var chunk in result.Chunks)
            {
                writer.TryWrite(chunk);
            }
            var timestamp = Stopwatch.GetTimestamp();
            if (timestamp - Interlocked.Read(ref _lastLevelsTimestamp) >= Stopwatch.Frequency / 25)
            {
                Interlocked.Exchange(ref _lastLevelsTimestamp, timestamp);
                LevelsChanged?.Invoke(this, result.Levels);
            }
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"音频触觉处理失败：{error.Message}");
        }
    }

    private void Capture_RecordingStopped(object? sender, StoppedEventArgs eventArgs)
    {
        if (eventArgs.Exception is not null && IsRunning)
        {
            Faulted?.Invoke(this, $"WASAPI 捕获已停止：{eventArgs.Exception.Message}");
        }
    }

    private sealed class HapticsProcessor
    {
        private readonly List<StereoFrame> _sourceFrames = [];
        private readonly List<byte[]> _completedChunks = [];
        private readonly Biquad _leftHighPass = new();
        private readonly Biquad _rightHighPass = new();
        private readonly Biquad _leftLowPass = new();
        private readonly Biquad _rightLowPass = new();
        private readonly Biquad[] _leftEq = [new(), new(), new(), new(), new(), new()];
        private readonly Biquad[] _rightEq = [new(), new(), new(), new(), new(), new()];
        private int _eqBandCount;
        private byte[] _chunk = new byte[BytesPerChunk];
        private AudioHapticsSettings? _appliedSettings;
        private WaveFormat? _format;
        private double _sourcePosition;
        private float _envelope;
        private float _gateGain;
        private int _chunkFrames;

        public void Reset(WaveFormat format)
        {
            _sourceFrames.Clear();
            _completedChunks.Clear();
            _chunk = new byte[BytesPerChunk];
            _format = format;
            _appliedSettings = null;
            _sourcePosition = 0;
            _envelope = 0;
            _gateGain = 0;
            _chunkFrames = 0;
            _leftHighPass.Reset();
            _rightHighPass.Reset();
            _leftLowPass.Reset();
            _rightLowPass.Reset();
            foreach (var band in _leftEq)
            {
                band.Reset();
            }
            foreach (var band in _rightEq)
            {
                band.Reset();
            }
        }

        public ProcessResult Process(
            byte[] input,
            int bytesRecorded,
            WaveFormat format,
            AudioHapticsSettings settings)
        {
            if (_format is null || _format.SampleRate != format.SampleRate ||
                _format.Channels != format.Channels || _format.BitsPerSample != format.BitsPerSample)
            {
                Reset(format);
            }
            ApplySettings(settings);
            _completedChunks.Clear();
            var inputLeftPeak = 0f;
            var inputRightPeak = 0f;
            var outputLeftPeak = 0f;
            var outputRightPeak = 0f;
            var bytesPerSample = Math.Max(1, format.BitsPerSample / 8);
            var frameBytes = bytesPerSample * Math.Max(1, format.Channels);
            var frames = bytesRecorded / frameBytes;
            for (var frame = 0; frame < frames; frame++)
            {
                var offset = frame * frameBytes;
                var left = ReadSample(input, offset, format, bytesPerSample);
                var right = format.Channels > 1
                    ? ReadSample(input, offset + bytesPerSample, format, bytesPerSample)
                    : left;
                inputLeftPeak = Math.Max(inputLeftPeak, Math.Abs(left));
                inputRightPeak = Math.Max(inputRightPeak, Math.Abs(right));
                _sourceFrames.Add(new StereoFrame(left, right));
            }

            if (_sourceFrames.Count < 2)
            {
                return new ProcessResult([], new AudioHapticsLevels(inputLeftPeak, inputRightPeak, 0, 0));
            }

            var step = format.SampleRate / (double)HapticsSampleRate;
            var speakerGain = settings.SpeakerEnabled
                ? (float)(Math.Clamp(settings.SpeakerVolumePercent, 0, 100) / 100)
                : 0f;
            while (_sourcePosition + 1 < _sourceFrames.Count)
            {
                var index = (int)_sourcePosition;
                var fraction = (float)(_sourcePosition - index);
                var current = _sourceFrames[index];
                var next = _sourceFrames[index + 1];
                var rawLeft = current.Left + (next.Left - current.Left) * fraction;
                var rawRight = current.Right + (next.Right - current.Right) * fraction;
                // Speaker channels carry the untouched capture; the haptics
                // DSP chain (filters, gate, compression) stays haptics-only.
                // Plain full-scale clamp instead of the tanh soft limiter so
                // the speaker keeps as much peak level as the DAC can use;
                // the preamp slider is the hardware loudness control now.
                var speakerLeft = Math.Clamp(rawLeft * speakerGain, -1f, 1f);
                var speakerRight = Math.Clamp(rawRight * speakerGain, -1f, 1f);
                ProcessFrame(rawLeft, rawRight, settings, out var hapticsLeft, out var hapticsRight);
                outputLeftPeak = Math.Max(outputLeftPeak, Math.Abs(hapticsLeft));
                outputRightPeak = Math.Max(outputRightPeak, Math.Abs(hapticsRight));
                WriteFrame(speakerLeft, speakerRight, hapticsLeft, hapticsRight);
                _sourcePosition += step;
            }
            var discard = Math.Max(0, (int)_sourcePosition - 1);
            if (discard > 0)
            {
                _sourceFrames.RemoveRange(0, discard);
                _sourcePosition -= discard;
            }

            return new ProcessResult(
                _completedChunks.ToArray(),
                new AudioHapticsLevels(inputLeftPeak, inputRightPeak, outputLeftPeak, outputRightPeak));
        }

        private void ApplySettings(AudioHapticsSettings settings)
        {
            if (ReferenceEquals(settings, _appliedSettings))
            {
                return;
            }
            _appliedSettings = settings;
            var low = Math.Clamp(settings.LowCutHz, 10, HapticsSampleRate / 2 - 10);
            var high = Math.Clamp(settings.HighCutHz, low + 10, HapticsSampleRate / 2 - 1);
            _leftHighPass.ConfigureHighPass(HapticsSampleRate, low);
            _rightHighPass.ConfigureHighPass(HapticsSampleRate, low);
            _leftLowPass.ConfigureLowPass(HapticsSampleRate, high);
            _rightLowPass.ConfigureLowPass(HapticsSampleRate, high);
            ConfigureEq(settings);
        }

        /// <summary>
        /// Configures the EQ chain: low shelf on the first band, peaking in
        /// the middle, high shelf on the last.
        /// </summary>
        private void ConfigureEq(AudioHapticsSettings settings)
        {
            _eqBandCount = Math.Min(settings.EqBandCount, _leftEq.Length);
            for (var band = 0; band < _eqBandCount; band++)
            {
                var frequency = settings.EqBandHz(band);
                var gainDb = Math.Clamp(settings.EqBandGainDb(band), -20, 20);
                var last = band == _eqBandCount - 1;
                foreach (var filter in new[] { _leftEq[band], _rightEq[band] })
                {
                    if (Math.Abs(gainDb) < .01)
                    {
                        filter.ConfigureBypass();
                    }
                    else if (band == 0)
                    {
                        filter.ConfigureLowShelf(HapticsSampleRate, frequency, gainDb);
                    }
                    else if (last)
                    {
                        filter.ConfigureHighShelf(HapticsSampleRate, frequency, gainDb);
                    }
                    else
                    {
                        filter.ConfigurePeaking(HapticsSampleRate, frequency, gainDb, 1.0);
                    }
                }
            }
        }

        private void ProcessFrame(
            float left,
            float right,
            AudioHapticsSettings settings,
            out float outputLeft,
            out float outputRight)
        {
            var gain = MathF.Pow(10, (float)settings.InputGainDb / 20);
            left *= gain;
            right *= gain;
            switch (settings.ChannelMode)
            {
                case "mono":
                    left = right = (left + right) * .5f;
                    break;
                case "left":
                    right = left;
                    break;
                case "right":
                    left = right;
                    break;
                case "swap":
                    (left, right) = (right, left);
                    break;
            }
            if (settings.ChannelMode is "stereo" or "swap")
            {
                var mid = (left + right) * .5f;
                var side = (left - right) * .5f * (float)(settings.StereoWidthPercent / 100);
                left = mid + side;
                right = mid - side;
            }

            left = _leftLowPass.Process(_leftHighPass.Process(left));
            right = _rightLowPass.Process(_rightHighPass.Process(right));
            // EQ sits pre-dynamics so the gate and compressor react to the
            // shaped signal.
            for (var band = 0; band < _eqBandCount; band++)
            {
                left = _leftEq[band].Process(left);
                right = _rightEq[band].Process(right);
            }
            var level = Math.Max(Math.Abs(left), Math.Abs(right));
            var attackCoefficient = TimeCoefficient(settings.AttackMs);
            var releaseCoefficient = TimeCoefficient(settings.ReleaseMs);
            var envelopeCoefficient = level > _envelope ? attackCoefficient : releaseCoefficient;
            _envelope += (level - _envelope) * envelopeCoefficient;
            var gateThreshold = MathF.Pow(10, (float)settings.GateDb / 20);
            var gateTarget = gateThreshold <= 0 || _envelope >= gateThreshold
                ? 1f
                : MathF.Pow(Math.Clamp(_envelope / gateThreshold, 0, 1), 2);
            _gateGain += (gateTarget - _gateGain) *
                (gateTarget > _gateGain ? attackCoefficient : releaseCoefficient);
            left *= _gateGain;
            right *= _gateGain;

            var ratio = (float)Math.Clamp(settings.CompressionRatio, 1, 12);
            left = Compress(left, ratio);
            right = Compress(right, ratio);
            // Pick the source stereo channel feeding each actuator
            // (0 = Left, 1 = Right).
            var sourceLeft = left;
            var sourceRight = right;
            left = settings.LeftMotorSource == 1 ? sourceRight : sourceLeft;
            right = settings.RightMotorSource == 1 ? sourceRight : sourceLeft;

            // Haptics gain: linear multiplier on the haptics signal.
            var hapticsGain = (float)Math.Clamp(settings.HapticsGain, .1, 10);
            left *= hapticsGain;
            right *= hapticsGain;

            left *= (float)(settings.StrengthPercent * settings.LeftPercent / 10_000);
            right *= (float)(settings.StrengthPercent * settings.RightPercent / 10_000);
            if (settings.InvertRight)
            {
                right = -right;
            }
            var ceiling = (float)Math.Clamp(settings.CeilingPercent / 100, .05, 1);
            outputLeft = SoftLimit(left, ceiling);
            outputRight = SoftLimit(right, ceiling);
        }

        private void WriteFrame(float speakerLeft, float speakerRight,
                                float hapticsLeft, float hapticsRight)
        {
            var offset = _chunkFrames * sizeof(short) * ChannelsPerFrame;
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset, sizeof(short)), ToInt16(speakerLeft));
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset + sizeof(short), sizeof(short)), ToInt16(speakerRight));
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset + sizeof(short) * 2, sizeof(short)), ToInt16(hapticsLeft));
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset + sizeof(short) * 3, sizeof(short)), ToInt16(hapticsRight));
            _chunkFrames++;
            if (_chunkFrames != FramesPerChunk)
            {
                return;
            }
            _completedChunks.Add(_chunk);
            _chunk = new byte[BytesPerChunk];
            _chunkFrames = 0;
        }

        private static float ReadSample(byte[] bytes, int offset, WaveFormat format, int bytesPerSample)
        {
            var isFloat = format.Encoding == WaveFormatEncoding.IeeeFloat ||
                          format.Encoding == WaveFormatEncoding.Extensible && format.BitsPerSample == 32;
            if (isFloat && bytesPerSample == 4)
            {
                return Math.Clamp(BitConverter.ToSingle(bytes, offset), -1f, 1f);
            }
            return bytesPerSample switch
            {
                2 => BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(offset, 2)) / 32768f,
                3 => ReadInt24(bytes, offset) / 8_388_608f,
                4 => BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(offset, 4)) / 2_147_483_648f,
                _ => 0f
            };
        }

        private static int ReadInt24(byte[] bytes, int offset)
        {
            var value = bytes[offset] | bytes[offset + 1] << 8 | bytes[offset + 2] << 16;
            return (value & 0x80_0000) != 0 ? value | unchecked((int)0xff00_0000) : value;
        }

        private static float TimeCoefficient(double milliseconds) =>
            1 - MathF.Exp(-1f / Math.Max(.0001f, (float)milliseconds / 1_000 * HapticsSampleRate));

        private static float Compress(float sample, float ratio)
        {
            const float threshold = .18f;
            var magnitude = Math.Abs(sample);
            if (magnitude <= threshold || ratio <= 1)
            {
                return sample;
            }
            var compressed = threshold + (magnitude - threshold) / ratio;
            return MathF.CopySign(compressed, sample);
        }

        private static float SoftLimit(float sample, float ceiling)
        {
            var limited = MathF.Tanh(sample / ceiling) * ceiling;
            return Math.Clamp(limited, -ceiling, ceiling);
        }

        private static short ToInt16(float sample) => (short)Math.Clamp(
            MathF.Round(sample * short.MaxValue), short.MinValue, short.MaxValue);

        private readonly record struct StereoFrame(float Left, float Right);

        public sealed record ProcessResult(IReadOnlyList<byte[]> Chunks, AudioHapticsLevels Levels);
    }

    private sealed class Biquad
    {
        private float _b0;
        private float _b1;
        private float _b2;
        private float _a1;
        private float _a2;
        private float _z1;
        private float _z2;

        public void Reset()
        {
            _z1 = 0;
            _z2 = 0;
        }

        public void ConfigureLowPass(int sampleRate, double frequency) => Configure(sampleRate, frequency, false);

        public void ConfigureHighPass(int sampleRate, double frequency) => Configure(sampleRate, frequency, true);

        public void ConfigureBypass()
        {
            _b0 = 1;
            _b1 = 0;
            _b2 = 0;
            _a1 = 0;
            _a2 = 0;
        }

        /// <summary>RBJ peaking EQ, used for the middle EQ bands.</summary>
        public void ConfigurePeaking(int sampleRate, double frequency, double gainDb, double q)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / (2 * Math.Max(.1, q));
            Normalize(
                1 + alpha * a, -2 * cosine, 1 - alpha * a,
                1 + alpha / a, -2 * cosine, 1 - alpha / a);
        }

        /// <summary>RBJ low shelf (S = 1), used for the lowest EQ band.</summary>
        public void ConfigureLowShelf(int sampleRate, double frequency, double gainDb)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / 2 * Math.Sqrt(2);
            var shared = 2 * Math.Sqrt(a) * alpha;
            Normalize(
                a * (a + 1 - (a - 1) * cosine + shared),
                2 * a * (a - 1 - (a + 1) * cosine),
                a * (a + 1 - (a - 1) * cosine - shared),
                a + 1 + (a - 1) * cosine + shared,
                -2 * (a - 1 + (a + 1) * cosine),
                a + 1 + (a - 1) * cosine - shared);
        }

        /// <summary>RBJ high shelf (S = 1), used for the highest EQ band.</summary>
        public void ConfigureHighShelf(int sampleRate, double frequency, double gainDb)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / 2 * Math.Sqrt(2);
            var shared = 2 * Math.Sqrt(a) * alpha;
            Normalize(
                a * (a + 1 + (a - 1) * cosine + shared),
                -2 * a * (a - 1 + (a + 1) * cosine),
                a * (a + 1 + (a - 1) * cosine - shared),
                a + 1 - (a - 1) * cosine + shared,
                2 * (a - 1 - (a + 1) * cosine),
                a + 1 - (a - 1) * cosine - shared);
        }

        private static double Omega(int sampleRate, double frequency) =>
            2 * Math.PI * Math.Clamp(frequency, 10, sampleRate / 2.0 - 1) / sampleRate;

        private void Normalize(double b0, double b1, double b2, double a0, double a1, double a2)
        {
            _b0 = (float)(b0 / a0);
            _b1 = (float)(b1 / a0);
            _b2 = (float)(b2 / a0);
            _a1 = (float)(a1 / a0);
            _a2 = (float)(a2 / a0);
        }

        public float Process(float sample)
        {
            var output = sample * _b0 + _z1;
            _z1 = sample * _b1 + _z2 - _a1 * output;
            _z2 = sample * _b2 - _a2 * output;
            return output;
        }

        private void Configure(int sampleRate, double frequency, bool highPass)
        {
            var omega = 2 * Math.PI * Math.Clamp(frequency, 10, sampleRate / 2.0 - 1) / sampleRate;
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / Math.Sqrt(2);
            var a0 = 1 + alpha;
            var b0 = highPass ? (1 + cosine) / 2 : (1 - cosine) / 2;
            var b1 = highPass ? -(1 + cosine) : 1 - cosine;
            var b2 = b0;
            _b0 = (float)(b0 / a0);
            _b1 = (float)(b1 / a0);
            _b2 = (float)(b2 / a0);
            _a1 = (float)(-2 * cosine / a0);
            _a2 = (float)((1 - alpha) / a0);
        }
    }
}
