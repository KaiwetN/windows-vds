using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace VdsGui;

public sealed class VdsBackend
{
    private const uint ScManagerConnect = 0x0001;
    private const uint ServiceQueryStatus = 0x0004;
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    public string? SourceRoot { get; } = FindSourceRoot();

    public SystemSnapshot GetSystemSnapshot()
    {
        var programFiles = Environment.GetEnvironmentVariable("ProgramW6432")
            ?? Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var installedVdsctl = Path.Combine(programFiles, "vDS", "vdsctl.exe");
        var localVdsctl = SourceRoot is null
            ? ""
            : Path.Combine(SourceRoot, "out", "build", "windows", "Release", "vdsctl.exe");
        var vdsctlPath = File.Exists(installedVdsctl) ? installedVdsctl : localVdsctl;
        var usbipInstalled = File.Exists(Path.Combine(programFiles, "USBip", "usbip.exe"));
        var hidHideInstalled = File.Exists(Path.Combine(
            programFiles,
            "Nefarius Software Solutions",
            "HidHide",
            "x64",
            "HidHideCLI.exe"));
        var installedDaemon = Path.Combine(programFiles, "vDS", "vdsd.exe");
        var localDaemon = SourceRoot is null
            ? ""
            : Path.Combine(SourceRoot, "out", "build", "windows", "Release", "vdsd.exe");
        var updateAvailable = FilesDiffer(installedDaemon, localDaemon);

        return new SystemSnapshot(
            QueryServiceState("vdsd"),
            usbipInstalled,
            hidHideInstalled,
            updateAvailable,
            SourceRoot is not null,
            vdsctlPath);
    }

    public async Task<IReadOnlyList<ControllerRow>> GetControllersAsync(
        string vdsctlPath,
        CancellationToken cancellationToken = default)
    {
        var targets = ParseJsonLines<ControllerTarget>(
            (await RunAsync(vdsctlPath, ["list-targets"], cancellationToken)).StandardOutput);
        var statuses = ParseJsonLines<ControllerStatus>(
            (await RunAsync(vdsctlPath, ["list"], cancellationToken)).StandardOutput);
        var statusByAddress = statuses.ToDictionary(
            item => item.Address,
            StringComparer.OrdinalIgnoreCase);
        var rows = new List<ControllerRow>();

        foreach (var target in targets)
        {
            statusByAddress.TryGetValue(target.Address, out var status);
            rows.Add(new ControllerRow
            {
                Name = string.IsNullOrWhiteSpace(target.Name) ? "DualSense" : target.Name,
                Address = target.Address,
                Online = target.Online,
                Registered = target.Registered || status is not null,
                Connected = status?.Connected ?? false,
                Endpoint = status?.Path ?? "",
                Profile = status?.Profile ?? ""
            });
            statusByAddress.Remove(target.Address);
        }

        foreach (var status in statusByAddress.Values)
        {
            rows.Add(new ControllerRow
            {
                Address = status.Address,
                Registered = true,
                Connected = status.Connected,
                Endpoint = status.Path,
                Profile = status.Profile
            });
        }
        return rows.OrderByDescending(item => item.Connected)
            .ThenByDescending(item => item.Online)
            .ThenBy(item => item.Address)
            .ToArray();
    }

    public async Task AttachAsync(
        string vdsctlPath,
        string address,
        string profile,
        string port,
        CancellationToken cancellationToken = default)
    {
        var arguments = new List<string> { "attach", address };
        if (port != "auto")
        {
            arguments.AddRange(["--ports", port]);
        }
        if (profile != "auto")
        {
            arguments.AddRange(["--profile", profile]);
        }
        var output = (await RunAsync(vdsctlPath, arguments, cancellationToken)).StandardOutput;
        var reply = ParseJsonLines<ControlReply>(output).SingleOrDefault();
        if (reply is null || !reply.Ok)
        {
            throw new InvalidOperationException(reply?.Error ?? "vdsctl 没有返回有效结果");
        }
    }

    public async Task DetachAsync(
        string vdsctlPath,
        string address,
        CancellationToken cancellationToken = default)
    {
        var output = (await RunAsync(
            vdsctlPath,
            ["detach", address],
            cancellationToken)).StandardOutput;
        var reply = ParseJsonLines<ControlReply>(output).SingleOrDefault();
        if (reply is null || !reply.Ok)
        {
            throw new InvalidOperationException(reply?.Error ?? "vdsctl 没有返回有效结果");
        }
    }

    public async Task<AudioBufferReply> GetAudioBufferAsync(
        string vdsctlPath,
        CancellationToken cancellationToken = default)
    {
        var output = (await RunAsync(
            vdsctlPath,
            ["audio-buffer"],
            cancellationToken)).StandardOutput;
        return RequireAudioBufferReply(output);
    }

    public async Task<AudioBufferReply> SetAudioBufferAsync(
        string vdsctlPath,
        int chunks,
        CancellationToken cancellationToken = default)
    {
        var output = (await RunAsync(
            vdsctlPath,
            ["audio-buffer", chunks.ToString(CultureInfo.InvariantCulture)],
            cancellationToken)).StandardOutput;
        return RequireAudioBufferReply(output);
    }

    public async Task ApplyEffectsAsync(
        string vdsctlPath,
        IReadOnlyList<string> arguments,
        CancellationToken cancellationToken = default)
    {
        var fullArguments = new List<string> { "effects" };
        fullArguments.AddRange(arguments);
        var output = (await RunAsync(vdsctlPath, fullArguments, cancellationToken)).StandardOutput;
        var reply = ParseJsonLines<ControlReply>(output).SingleOrDefault();
        if (reply is null || !reply.Ok)
        {
            throw new InvalidOperationException(reply?.Error ?? "vdsctl 没有返回有效结果");
        }
    }

    public async Task StartServiceAsync(CancellationToken cancellationToken = default)
    {
        await RunAsync("sc.exe", ["start", "vdsd"], cancellationToken);
    }

    public async Task<string> InstallOrUpdateAsync(CancellationToken cancellationToken = default)
    {
        if (SourceRoot is null)
        {
            throw new InvalidOperationException("找不到源码目录，无法运行安装脚本");
        }
        var script = Path.Combine(SourceRoot, "setup-windows.ps1");
        var result = await RunAsync(
            "powershell.exe",
            ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-Mode", "Install"],
            cancellationToken,
            TimeSpan.FromMinutes(20));
        return string.IsNullOrWhiteSpace(result.StandardOutput)
            ? "安装完成"
            : result.StandardOutput.Trim();
    }

    /// <summary>
    /// Selects the 0x36 native voice-coil waveform report over the 0x31 rumble
    /// fallback. The daemon reads this once at startup, so changing it requires
    /// a service restart.
    /// </summary>
    public static string HapticsModePath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "vDS",
        "haptics-mode.txt");

    public static bool IsNativeHaptics()
    {
        try
        {
            var path = HapticsModePath();
            return File.Exists(path) &&
                   File.ReadAllText(path).Trim()
                       .Equals("native", StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    public async Task SetHapticsModeAsync(
        bool native,
        CancellationToken cancellationToken = default)
    {
        var path = HapticsModePath();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, native ? "native" : "rumble");
        await RestartDaemonAsync(cancellationToken);
    }

    public async Task RestartDaemonAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            // Already-stopped reports 1062, which is not an error here.
            await RunAsync("sc.exe", ["stop", "vdsd"], cancellationToken, TimeSpan.FromSeconds(45));
        }
        catch (InvalidOperationException)
        {
        }

        // sc.exe returns as soon as the control is accepted, so starting
        // immediately can race a still-stopping service.
        for (var attempt = 0; attempt < 40; attempt++)
        {
            if (QueryServiceState("vdsd") == VdsServiceState.Stopped)
            {
                break;
            }
            await Task.Delay(250, cancellationToken);
        }

        await RunAsync("sc.exe", ["start", "vdsd"], cancellationToken, TimeSpan.FromSeconds(45));
    }

    public void OpenBluetoothSettings() => OpenShell("ms-settings:bluetooth");

    public void OpenGameControllers() => OpenShell("joy.cpl");

    public void OpenLog()
    {
        var path = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "vDS",
            "vdsd.log");
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("vDS 日志尚未生成", path);
        }
        Process.Start(new ProcessStartInfo("notepad.exe", path) { UseShellExecute = true });
    }

    private IReadOnlyList<T> ParseJsonLines<T>(string text)
    {
        var values = new List<T>();
        foreach (var line in text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
        {
            var value = JsonSerializer.Deserialize<T>(line, _jsonOptions);
            if (value is not null)
            {
                values.Add(value);
            }
        }
        return values;
    }

    private AudioBufferReply RequireAudioBufferReply(string output)
    {
        var reply = ParseJsonLines<AudioBufferReply>(output).SingleOrDefault();
        if (reply is null || !reply.Ok)
        {
            throw new InvalidOperationException(reply?.Error ?? "vdsctl 没有返回有效的缓冲设置");
        }
        return reply;
    }

    private static async Task<ProcessResult> RunAsync(
        string fileName,
        IEnumerable<string> arguments,
        CancellationToken cancellationToken,
        TimeSpan? timeout = null)
    {
        if (string.IsNullOrWhiteSpace(fileName) ||
            (Path.IsPathRooted(fileName) && !File.Exists(fileName)))
        {
            throw new FileNotFoundException("找不到所需程序", fileName);
        }
        var startInfo = new ProcessStartInfo(fileName)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }
        using var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            throw new InvalidOperationException($"无法启动 {fileName}");
        }
        var standardOutput = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var standardError = process.StandardError.ReadToEndAsync(cancellationToken);
        using var timeoutSource = new CancellationTokenSource(timeout ?? TimeSpan.FromSeconds(15));
        using var linkedSource = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            timeoutSource.Token);
        try
        {
            await process.WaitForExitAsync(linkedSource.Token);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited)
            {
                process.Kill(true);
            }
            if (timeoutSource.IsCancellationRequested)
            {
                throw new TimeoutException($"{Path.GetFileName(fileName)} 操作超时");
            }
            throw;
        }
        var result = new ProcessResult(
            process.ExitCode,
            await standardOutput,
            await standardError);
        if (result.ExitCode != 0)
        {
            var detail = string.IsNullOrWhiteSpace(result.StandardError)
                ? result.StandardOutput
                : result.StandardError;
            throw new InvalidOperationException(
                $"{Path.GetFileName(fileName)} 失败（{result.ExitCode}）：{detail.Trim()}");
        }
        return result;
    }

    private static void OpenShell(string target)
    {
        Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
    }

    private static string? FindSourceRoot()
    {
        var markerPath = Path.Combine(AppContext.BaseDirectory, "vds-source-root.txt");
        if (File.Exists(markerPath))
        {
            var markedRoot = File.ReadAllText(markerPath).Trim();
            if (File.Exists(Path.Combine(markedRoot, "setup-windows.ps1")) &&
                File.Exists(Path.Combine(markedRoot, "VERSION")))
            {
                return markedRoot;
            }
        }
        var starts = new[] { AppContext.BaseDirectory, Environment.CurrentDirectory };
        foreach (var start in starts)
        {
            var directory = new DirectoryInfo(start);
            for (var depth = 0; directory is not null && depth < 8; depth++)
            {
                if (File.Exists(Path.Combine(directory.FullName, "setup-windows.ps1")) &&
                    File.Exists(Path.Combine(directory.FullName, "VERSION")))
                {
                    return directory.FullName;
                }
                directory = directory.Parent;
            }
        }
        return null;
    }

    private static bool FilesDiffer(string installedPath, string localPath)
    {
        if (!File.Exists(localPath))
        {
            return false;
        }
        if (!File.Exists(installedPath))
        {
            return true;
        }
        using var installed = File.OpenRead(installedPath);
        using var local = File.OpenRead(localPath);
        return !SHA256.HashData(installed).SequenceEqual(SHA256.HashData(local));
    }

    private static VdsServiceState QueryServiceState(string serviceName)
    {
        var manager = OpenSCManager(null, null, ScManagerConnect);
        if (manager == IntPtr.Zero)
        {
            return VdsServiceState.Unknown;
        }
        try
        {
            var service = OpenService(manager, serviceName, ServiceQueryStatus);
            if (service == IntPtr.Zero)
            {
                return Marshal.GetLastWin32Error() == 1060
                    ? VdsServiceState.NotInstalled
                    : VdsServiceState.Unknown;
            }
            try
            {
                if (!QueryServiceStatus(service, out var status))
                {
                    return VdsServiceState.Unknown;
                }
                return status.CurrentState switch
                {
                    1 => VdsServiceState.Stopped,
                    2 => VdsServiceState.StartPending,
                    3 => VdsServiceState.StopPending,
                    4 => VdsServiceState.Running,
                    _ => VdsServiceState.Unknown
                };
            }
            finally
            {
                CloseServiceHandle(service);
            }
        }
        finally
        {
            CloseServiceHandle(manager);
        }
    }

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenSCManager(
        string? machineName,
        string? databaseName,
        uint desiredAccess);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenService(
        IntPtr manager,
        string serviceName,
        uint desiredAccess);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryServiceStatus(
        IntPtr service,
        out ServiceStatus status);

    [DllImport("advapi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseServiceHandle(IntPtr handle);

    [StructLayout(LayoutKind.Sequential)]
    private struct ServiceStatus
    {
        public uint ServiceType;
        public uint CurrentState;
        public uint ControlsAccepted;
        public uint Win32ExitCode;
        public uint ServiceSpecificExitCode;
        public uint CheckPoint;
        public uint WaitHint;
    }

    private sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError);
}
