using System.Collections.ObjectModel;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace VdsGui;

public partial class MainWindow : Window
{
    private static readonly Brush SuccessBackground = BrushFrom("#DCFCE7");
    private static readonly Brush SuccessForeground = BrushFrom("#16794B");
    private static readonly Brush WarningBackground = BrushFrom("#FEF3C7");
    private static readonly Brush WarningForeground = BrushFrom("#9A6700");
    private static readonly Brush ErrorBackground = BrushFrom("#FEE2E2");
    private static readonly Brush ErrorForeground = BrushFrom("#B42318");
    private static readonly Brush NeutralBackground = BrushFrom("#EEF2F7");
    private static readonly Brush NeutralForeground = BrushFrom("#596780");
    private readonly VdsBackend _backend = new();
    private readonly AudioHapticsEngine _audioHaptics = new();
    private readonly DispatcherTimer _timer;
    private readonly DispatcherTimer _audioHapticsSettingsSaveTimer;
    private SystemSnapshot? _snapshot;
    private bool _refreshing;
    private bool _busy;
    private bool _speakerEnabledBeforeControllerCapture;
    private bool _audioBufferLoaded;
    private int _appliedAudioBufferChunks = 3;
    private AudioHapticsSettings _audioHapticsSettings = new();
    private bool _audioHapticsUiLoading;
    private bool _audioHapticsUiReady;
    private bool _audioHapticsAutoStartAttempted;
    private readonly DispatcherTimer _effectsApplyTimer;
    private EffectsSettings _effectsSettings = new();
    private bool _effectsUiLoading;
    private bool _effectsUiReady;
    private bool _effectsStartupApplied;

    public ObservableCollection<ControllerRow> Controllers { get; } = [];

    public MainWindow()
    {
        InitializeComponent();
        DataContext = this;
        SourceInitialized += (_, _) => FitIntoWorkArea();
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
        _audioHapticsSettingsSaveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(450) };
        _audioHapticsSettingsSaveTimer.Tick += (_, _) => SaveAudioHapticsSettings();
        _effectsApplyTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
        _effectsApplyTimer.Tick += async (_, _) =>
        {
            _effectsApplyTimer.Stop();
            await ApplyEffectsToDaemonAsync();
        };
        InitializeEffectsUi();
        InitializeAudioHapticsUi();
        _audioHapticsUiReady = true;
        ApplyAudioHapticsSettingsFromUi(false);
        _audioHaptics.LevelsChanged += AudioHaptics_LevelsChanged;
        _audioHaptics.Faulted += AudioHaptics_Faulted;
        _timer.Tick += async (_, _) => await RefreshAsync(false);
        Loaded += async (_, _) =>
        {
            _timer.Start();
            await RefreshAsync(true);
            // Re-apply saved effects once per launch, but only if the panel
            // has been used before - a fresh install must not clear effects
            // configured through vdsctl.
            if (!_effectsStartupApplied && EffectsSettings.SettingsFileExists())
            {
                _effectsStartupApplied = true;
                await ApplyEffectsToDaemonAsync();
            }
            if (_audioHapticsSettings.AutoStart && !_audioHapticsAutoStartAttempted)
            {
                _audioHapticsAutoStartAttempted = true;
                await StartAudioHapticsAsync(false);
            }
        };
        Closed += async (_, _) =>
        {
            _timer.Stop();
            _audioHapticsSettingsSaveTimer.Stop();
            SaveAudioHapticsSettings();
            await _audioHaptics.DisposeAsync();
        };
    }

    /// <summary>
    /// Keeps the window inside the monitor work area. Without this the default
    /// 1320x850 size overflows a 1536x864 DIP desktop (a 1080p panel at 125%
    /// scaling) once the taskbar is subtracted, and the window can land
    /// partly or wholly off-screen.
    /// </summary>
    private void FitIntoWorkArea()
    {
        var area = SystemParameters.WorkArea;
        if (area.Width <= 0 || area.Height <= 0)
        {
            return;
        }

        var width = Math.Min(Width, area.Width);
        var height = Math.Min(Height, area.Height);
        if (width < MinWidth)
        {
            MinWidth = width;
        }
        if (height < MinHeight)
        {
            MinHeight = height;
        }
        Width = width;
        Height = height;

        var left = Math.Clamp(Left, area.Left, Math.Max(area.Left, area.Right - width));
        var top = Math.Clamp(Top, area.Top, Math.Max(area.Top, area.Bottom - height));
        if (double.IsNaN(Left) || double.IsNaN(Top) ||
            Left < area.Left || Top < area.Top ||
            Left + width > area.Right || Top + height > area.Bottom)
        {
            Left = left;
            Top = top;
        }
    }

    private async Task RefreshAsync(bool showErrors)
    {
        if (_refreshing || _busy)
        {
            return;
        }
        _refreshing = true;
        var selectedAddress = (ControllerGrid.SelectedItem as ControllerRow)?.Address;
        try
        {
            _snapshot = _backend.GetSystemSnapshot();
            UpdateSystemStatus(_snapshot);
            IReadOnlyList<ControllerRow> rows = [];
            if (_snapshot.ServiceState == VdsServiceState.Running &&
                !string.IsNullOrWhiteSpace(_snapshot.VdsctlPath))
            {
                rows = await _backend.GetControllersAsync(_snapshot.VdsctlPath);
                if (!_snapshot.UpdateAvailable)
                {
                    var audioBuffer = await _backend.GetAudioBufferAsync(_snapshot.VdsctlPath);
                    if (!AudioBufferSlider.IsMouseCaptureWithin)
                    {
                        UpdateAudioBufferUi(audioBuffer);
                    }
                }
            }

            Controllers.Clear();
            foreach (var row in rows)
            {
                Controllers.Add(row);
            }
            ControllerCountText.Text = rows.Count == 0
                ? "没有可显示的已配对手柄"
                : $"发现 {rows.Count} 个手柄，状态每 3 秒自动刷新";
            EmptyPanel.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            ControllerGrid.Visibility = rows.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
            ControllerGrid.SelectedItem = rows.FirstOrDefault(item =>
                string.Equals(item.Address, selectedAddress, StringComparison.OrdinalIgnoreCase))
                ?? rows.FirstOrDefault();
            UpdateActionButtons();
            RefreshAudioHapticsTargets();
            LastRefreshText.Text = $"上次刷新 {DateTime.Now:HH:mm:ss}";

            if (_snapshot.UpdateAvailable)
            {
                SetActivity("检测到修复版核心，点击“安装 / 更新 vDS”后即可识别在线手柄。", WarningForeground);
            }
            else if (rows.Any(item => item.Connected))
            {
                SetActivity("虚拟有线 DualSense 已连接，可以启动游戏。", SuccessForeground);
            }
            else if (rows.Any(item => item.Registered && item.Online))
            {
                SetActivity("手柄在线，正在等待虚拟 USB 桥建立。", WarningForeground);
            }
            else if (rows.Any(item => item.Registered))
            {
                SetActivity("手柄已注册；按 PS 键唤醒后会自动连接。", NeutralForeground);
            }
            else
            {
                SetActivity("选择已配对手柄，然后点击“注册并连接”。", NeutralForeground);
            }
        }
        catch (Exception error)
        {
            SetActivity(SimplifyError(error), ErrorForeground);
            if (showErrors)
            {
                ShowError(error);
            }
        }
        finally
        {
            _refreshing = false;
        }
    }

    private void UpdateSystemStatus(SystemSnapshot snapshot)
    {
        var serviceText = snapshot.ServiceState switch
        {
            VdsServiceState.Running => "运行中",
            VdsServiceState.Stopped => "已停止",
            VdsServiceState.StartPending => "启动中",
            VdsServiceState.StopPending => "停止中",
            VdsServiceState.NotInstalled => "未安装",
            _ => "未知"
        };
        SetPill(
            ServicePill,
            ServiceStatusText,
            serviceText,
            snapshot.ServiceState == VdsServiceState.Running,
            snapshot.ServiceState == VdsServiceState.StartPending);
        SetPill(UsbipPill, UsbipStatusText, snapshot.UsbipInstalled ? "已安装" : "缺失", snapshot.UsbipInstalled);
        SetPill(HidHidePill, HidHideStatusText, snapshot.HidHideInstalled ? "已安装" : "缺失", snapshot.HidHideInstalled);
        // "可更新" is informational, not a failure - render it amber, not red.
        SetPill(CorePill, CoreStatusText, snapshot.UpdateAvailable ? "可更新" : "最新",
            !snapshot.UpdateAvailable, snapshot.UpdateAvailable);
        // Gate by enablement, not visibility: collapsing the button removed it
        // from the stack and shifted the whole column on every 3 s poll.
        StartServiceButton.IsEnabled = snapshot.ServiceState == VdsServiceState.Stopped && !_busy;
        UpdateButton.IsEnabled = snapshot.SourceAvailable && !_busy;
        UpdateButton.Content = snapshot.ServiceState == VdsServiceState.NotInstalled
            ? "安装 vDS 与驱动"
            : snapshot.UpdateAvailable ? "安装修复版核心" : "重新安装 / 修复";
        UpdateAudioBufferAvailability();
        UpdateAudioHapticsAvailability();
    }

    private void UpdateAudioBufferAvailability()
    {
        var available = _snapshot?.ServiceState == VdsServiceState.Running &&
                        !_snapshot.UpdateAvailable && !_busy;
        AudioBufferSlider.IsEnabled = available;
        AudioBufferApplyButton.IsEnabled = available;
        if (!available)
        {
            AudioBufferStatusText.Text = _snapshot?.UpdateAvailable == true
                ? "安装新版核心后即可实时调节"
                : "启动 vDS 服务后即可调节";
        }
    }

    private void UpdateAudioHapticsAvailability()
    {
        if (AudioHapticsToggleButton is null)
        {
            return;
        }
        var serviceReady = _snapshot?.ServiceState == VdsServiceState.Running &&
                           _snapshot.UpdateAvailable != true &&
                           !_busy;
        var usbTarget = (AudioHapticsTargetCombo.SelectedItem as AudioHapticsTarget)?.Mode
            is "usb_audio" or "usb_rumble" or "bt_sbc";
        AudioHapticsToggleButton.IsEnabled = _audioHaptics.IsRunning ||
                                              ((serviceReady || usbTarget) &&
                                               AudioHapticsDeviceCombo.Items.Count > 0);
        AudioHapticsDeviceCombo.IsEnabled = !_audioHaptics.IsRunning && !_busy;
        AudioHapticsTargetCombo.IsEnabled = !_audioHaptics.IsRunning && !_busy;
        AudioHapticsToggleButton.Content = _audioHaptics.IsRunning
            ? "停止音频触觉"
            : "开始音频触觉";
        if (!serviceReady && !_audioHaptics.IsRunning)
        {
            SetAudioHapticsStatus(
                _snapshot?.UpdateAvailable == true
                    ? "请先安装新版核心以启用桌面音频触觉。"
                    : "启动 vDS 服务后即可将桌面音频映射到手柄马达。",
                NeutralForeground);
        }
    }

    private void UpdateAudioBufferUi(AudioBufferReply reply)
    {
        _audioBufferLoaded = true;
        _appliedAudioBufferChunks = reply.Chunks;
        AudioBufferSlider.Value = reply.Chunks;
        AudioBufferStatusText.Text = $"当前生效：{reply.Milliseconds} ms";
    }

    private static void SetPill(
        Border border,
        TextBlock textBlock,
        string text,
        bool success,
        bool pending = false)
    {
        textBlock.Text = text;
        border.Background = success
            ? SuccessBackground
            : pending ? WarningBackground : ErrorBackground;
        textBlock.Foreground = success
            ? SuccessForeground
            : pending ? WarningForeground : ErrorForeground;
    }

    private void UpdateActionButtons()
    {
        var selected = ControllerGrid.SelectedItem as ControllerRow;
        AttachButton.IsEnabled = selected is not null && !selected.Registered &&
                                 !selected.IsUsb && !_busy;
        DetachButton.IsEnabled = selected is not null && selected.Registered && !_busy;
        ProfileCombo.IsEnabled = selected is not null && !selected.Registered && !_busy;
        PortCombo.IsEnabled = selected is not null && !selected.Registered && !_busy;
    }

    private async void Attach_Click(object sender, RoutedEventArgs e)
    {
        if (ControllerGrid.SelectedItem is not ControllerRow selected || _snapshot is null)
        {
            return;
        }
        var profile = ((ComboBoxItem)ProfileCombo.SelectedItem).Tag?.ToString() ?? "auto";
        var port = ((ComboBoxItem)PortCombo.SelectedItem).Tag?.ToString() ?? "auto";
        await RunActionAsync(
            "正在注册并连接手柄...",
            async () => await _backend.AttachAsync(_snapshot.VdsctlPath, selected.Address, profile, port),
            selected.Online
                ? "手柄注册成功，正在建立虚拟 USB。"
                : "手柄注册成功；按 PS 键唤醒后会自动连接。");
    }

    private async void Detach_Click(object sender, RoutedEventArgs e)
    {
        if (ControllerGrid.SelectedItem is not ControllerRow selected || _snapshot is null)
        {
            return;
        }
        var answer = MessageBox.Show(
            $"确定取消注册 {selected.Name}？\n\n{selected.Address}",
            "取消注册",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);
        if (answer != MessageBoxResult.Yes)
        {
            return;
        }
        await RunActionAsync(
            "正在取消注册...",
            async () => await _backend.DetachAsync(_snapshot.VdsctlPath, selected.Address),
            "手柄已取消注册。");
    }

    private async void Update_Click(object sender, RoutedEventArgs e)
    {
        var needsDrivers = _snapshot is null || !_snapshot.UsbipInstalled || !_snapshot.HidHideInstalled;
        var warning = needsDrivers
            ? "将安装 USB/IP、HidHide 和 vDS 服务。安装 USB/IP 时 USB 设备可能短暂断开。"
            : "将停止 vDS 服务数秒并替换为刚刚编译的修复版，然后自动重启服务。";
        var answer = MessageBox.Show(
            warning + "\n\n是否继续？",
            "安装 / 更新 vDS",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (answer != MessageBoxResult.Yes)
        {
            return;
        }
        await RunActionAsync(
            "正在安装并更新 vDS，可能需要几十秒...",
            async () => { await _backend.InstallOrUpdateAsync(); },
            "vDS 已更新，正在重新检测手柄。");
    }

    private async void StartService_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync(
            "正在启动桥接服务...",
            async () => await _backend.StartServiceAsync(),
            "vDS 服务已启动。");
    }

    private void AudioBufferSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (AudioBufferValueText is null)
        {
            return;
        }
        var chunks = Math.Clamp((int)Math.Round(e.NewValue), 1, 48);
        var milliseconds = (int)Math.Round(chunks * 512000.0 / 48000.0);
        AudioBufferValueText.Text = $"{milliseconds} ms · {chunks} 块";
        if (_audioBufferLoaded && AudioBufferStatusText is not null)
        {
            AudioBufferStatusText.Text = chunks == _appliedAudioBufferChunks
                ? $"当前生效：{milliseconds} ms"
                : "已修改，点击“立即应用”";
        }
    }

    private async void AudioBufferApply_Click(object sender, RoutedEventArgs e)
    {
        if (_snapshot?.ServiceState != VdsServiceState.Running ||
            string.IsNullOrWhiteSpace(_snapshot.VdsctlPath))
        {
            return;
        }
        var chunks = Math.Clamp((int)Math.Round(AudioBufferSlider.Value), 1, 48);
        AudioBufferApplyButton.IsEnabled = false;
        try
        {
            var reply = await _backend.SetAudioBufferAsync(_snapshot.VdsctlPath, chunks);
            UpdateAudioBufferUi(reply);
            SetActivity($"音频 / 震动缓冲已实时调整为 {reply.Milliseconds} ms。", SuccessForeground);
        }
        catch (Exception error)
        {
            SetActivity(SimplifyError(error), ErrorForeground);
            ShowError(error);
        }
        finally
        {
            UpdateAudioBufferAvailability();
        }
    }

    private async Task RunActionAsync(string busyText, Func<Task> action, string successText)
    {
        SetBusy(true, busyText);
        try
        {
            await action();
            SetActivity(successText, SuccessForeground);
        }
        catch (Exception error)
        {
            SetActivity(SimplifyError(error), ErrorForeground);
            ShowError(error);
        }
        finally
        {
            SetBusy(false, "");
        }
        await Task.Delay(600);
        await RefreshAsync(false);
    }

    private void SetBusy(bool busy, string text)
    {
        _busy = busy;
        BusyText.Text = text;
        BusyOverlay.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        UpdateActionButtons();
        if (_snapshot is not null)
        {
            UpdateButton.IsEnabled = _snapshot.SourceAvailable && !busy;
        }
        UpdateAudioBufferAvailability();
        UpdateAudioHapticsAvailability();
    }

    private void SetActivity(string text, Brush color)
    {
        ActivityText.Text = text;
        ActivityText.Foreground = color;
        ActivityDot.Fill = color;
    }

    private static string SimplifyError(Exception error)
    {
        var text = error.Message.Trim();
        return text.Length > 220 ? text[..220] + "…" : text;
    }

    private void ShowError(Exception error)
    {
        MessageBox.Show(
            SimplifyError(error),
            "vDS 操作失败",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await RefreshAsync(true);

    private void ControllerGrid_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateActionButtons();

    private void BluetoothSettings_Click(object sender, RoutedEventArgs e) => RunShellAction(_backend.OpenBluetoothSettings);

    private void GameControllers_Click(object sender, RoutedEventArgs e) => RunShellAction(_backend.OpenGameControllers);

    private void OpenLog_Click(object sender, RoutedEventArgs e) => RunShellAction(_backend.OpenLog);

    private void InitializeEffectsUi()
    {
        _effectsSettings = EffectsSettings.Load();
        _effectsUiLoading = true;
        SelectComboItemByTag(EffectsLeftTriggerCombo, _effectsSettings.LeftTrigger);
        SelectComboItemByTag(EffectsRightTriggerCombo, _effectsSettings.RightTrigger);
        EffectsTriggerStrengthSlider.Value = Math.Clamp(_effectsSettings.TriggerStrength, 1, 8);
        EffectsLightbarCheckBox.IsChecked = _effectsSettings.LightbarEnabled;
        EffectsLightbarRSlider.Value = Math.Clamp(_effectsSettings.LightbarR, 0, 255);
        EffectsLightbarGSlider.Value = Math.Clamp(_effectsSettings.LightbarG, 0, 255);
        EffectsLightbarBSlider.Value = Math.Clamp(_effectsSettings.LightbarB, 0, 255);
        SelectComboItemByTag(EffectsPlayerCombo, _effectsSettings.PlayerLights);
        SelectComboItemByTag(EffectsMuteLedCombo, _effectsSettings.MuteLed);
        EffectsForceCheckBox.IsChecked = _effectsSettings.Force;
        _effectsUiLoading = false;
        _effectsUiReady = true;
        UpdateEffectsReadouts();
    }

    private void Effects_ValueChanged(object sender, RoutedEventArgs e) => EffectsUiChanged();

    private void Effects_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => EffectsUiChanged();

    private void Effects_ValueChanged(object sender, SelectionChangedEventArgs e) => EffectsUiChanged();

    private void EffectsUiChanged()
    {
        if (!_effectsUiReady || _effectsUiLoading)
        {
            return;
        }
        _effectsSettings = new EffectsSettings
        {
            LeftTrigger = (EffectsLeftTriggerCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "follow",
            RightTrigger = (EffectsRightTriggerCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "follow",
            TriggerStrength = (int)Math.Round(EffectsTriggerStrengthSlider.Value),
            LightbarEnabled = EffectsLightbarCheckBox.IsChecked == true,
            LightbarR = (int)Math.Round(EffectsLightbarRSlider.Value),
            LightbarG = (int)Math.Round(EffectsLightbarGSlider.Value),
            LightbarB = (int)Math.Round(EffectsLightbarBSlider.Value),
            PlayerLights = (EffectsPlayerCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "follow",
            MuteLed = (EffectsMuteLedCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "follow",
            Force = EffectsForceCheckBox.IsChecked == true
        };
        UpdateEffectsReadouts();
        try
        {
            _effectsSettings.Save();
        }
        catch
        {
        }
        _effectsApplyTimer.Stop();
        _effectsApplyTimer.Start();
    }

    private void UpdateEffectsReadouts()
    {
        EffectsTriggerStrengthText.Text = $"{_effectsSettings.TriggerStrength} / 8";
        EffectsLightbarPanel.Visibility = _effectsSettings.LightbarEnabled
            ? Visibility.Visible
            : Visibility.Collapsed;
        EffectsLightbarPreview.Background = new SolidColorBrush(Color.FromRgb(
            (byte)Math.Clamp(_effectsSettings.LightbarR, 0, 255),
            (byte)Math.Clamp(_effectsSettings.LightbarG, 0, 255),
            (byte)Math.Clamp(_effectsSettings.LightbarB, 0, 255)));
    }

    private async Task ApplyEffectsToDaemonAsync()
    {
        var vdsctlPath = _snapshot?.VdsctlPath;
        if (string.IsNullOrWhiteSpace(vdsctlPath))
        {
            EffectsStatusText.Text = "找不到 vdsctl，无法应用扳机与灯效。";
            EffectsStatusText.Foreground = ErrorForeground;
            return;
        }
        try
        {
            await _backend.ApplyEffectsAsync(vdsctlPath, _effectsSettings.BuildVdsctlArguments());
            EffectsStatusText.Text = "已应用到手柄；「跟随游戏」表示不干预对应功能。";
            EffectsStatusText.Foreground = NeutralForeground;
        }
        catch (Exception error)
        {
            EffectsStatusText.Text = $"应用失败：{SimplifyError(error)}";
            EffectsStatusText.Foreground = ErrorForeground;
        }
    }

    private void InitializeAudioHapticsUi()
    {
        _audioHapticsSettings = AudioHapticsSettings.Load();
        _audioHapticsUiLoading = true;
        AudioHapticsAutoStartCheckBox.IsChecked = _audioHapticsSettings.AutoStart;
        AudioHapticsStrengthSlider.Value = _audioHapticsSettings.StrengthPercent;
        AudioHapticsGainSlider.Value = _audioHapticsSettings.InputGainDb;
        AudioHapticsLowCutSlider.Value = _audioHapticsSettings.LowCutHz;
        AudioHapticsHighCutSlider.Value = _audioHapticsSettings.HighCutHz;
        AudioHapticsGateSlider.Value = _audioHapticsSettings.GateDb;
        AudioHapticsCompressionSlider.Value = _audioHapticsSettings.CompressionRatio;
        AudioHapticsAttackSlider.Value = _audioHapticsSettings.AttackMs;
        AudioHapticsReleaseSlider.Value = _audioHapticsSettings.ReleaseMs;
        AudioHapticsWidthSlider.Value = _audioHapticsSettings.StereoWidthPercent;
        AudioHapticsUsbLatencySlider.Value = Math.Clamp(_audioHapticsSettings.UsbLatencyMs, 30, 300);
        AudioHapticsCaptureLatencySlider.Value = Math.Clamp(_audioHapticsSettings.CaptureLatencyMs, 10, 200);
        AudioHapticsPlaybackQueueSlider.Value = Math.Clamp(_audioHapticsSettings.PlaybackQueueMs, 20, 300);
        AudioHapticsLeftSlider.Value = _audioHapticsSettings.LeftPercent;
        AudioHapticsRightSlider.Value = _audioHapticsSettings.RightPercent;
        AudioHapticsCeilingSlider.Value = _audioHapticsSettings.CeilingPercent;
        AudioHapticsInvertRightCheckBox.IsChecked = _audioHapticsSettings.InvertRight;
        SelectComboItemByTag(AudioHapticsChannelModeCombo, _audioHapticsSettings.ChannelMode);
        AudioHapticsSpeakerCheckBox.IsChecked = _audioHapticsSettings.SpeakerEnabled;
        AudioHapticsSpeakerVolumeSlider.Value = Math.Clamp(_audioHapticsSettings.SpeakerVolumePercent, 0, 100);
        AudioHapticsSpeakerPreampSlider.Value = Math.Clamp(_audioHapticsSettings.SpeakerPreamp, 0, 7);
        AudioHapticsVoiceCoilGainSlider.Value = _audioHapticsSettings.HapticsGain;
        SelectComboItemByTag(AudioHapticsLeftSourceCombo, _audioHapticsSettings.LeftMotorSource.ToString());
        SelectComboItemByTag(AudioHapticsRightSourceCombo, _audioHapticsSettings.RightMotorSource.ToString());
        SelectComboItemByTag(AudioHapticsEqModeCombo, _audioHapticsSettings.EqMode);
        AudioHapticsEq3Band1Slider.Value = _audioHapticsSettings.Eq3Band1GainDb;
        AudioHapticsEq3Band2Slider.Value = _audioHapticsSettings.Eq3Band2GainDb;
        AudioHapticsEq3Band3Slider.Value = _audioHapticsSettings.Eq3Band3GainDb;
        AudioHapticsEq6Band1Slider.Value = _audioHapticsSettings.Eq6Band1GainDb;
        AudioHapticsEq6Band2Slider.Value = _audioHapticsSettings.Eq6Band2GainDb;
        AudioHapticsEq6Band3Slider.Value = _audioHapticsSettings.Eq6Band3GainDb;
        AudioHapticsEq6Band4Slider.Value = _audioHapticsSettings.Eq6Band4GainDb;
        AudioHapticsEq6Band5Slider.Value = _audioHapticsSettings.Eq6Band5GainDb;
        AudioHapticsEq6Band6Slider.Value = _audioHapticsSettings.Eq6Band6GainDb;
        SelectComboItemByTag(AudioHapticsPresetCombo, "custom");
        SelectComboItemByTag(AudioHapticsModeCombo, VdsBackend.IsNativeHaptics() ? "native" : "rumble");
        _audioHapticsUiLoading = false;
        UpdateHapticsModeHint();
        try
        {
            RefreshAudioHapticsDevices();
            RefreshAudioHapticsTargets();
            UpdateAudioHapticsSpeakerGuard();
        }
        catch (Exception error)
        {
            SetAudioHapticsStatus($"无法枚举播放设备：{SimplifyError(error)}", ErrorForeground);
        }
    }

    private void RefreshAudioHapticsDevices()
    {
        var selectedId = _audioHapticsSettings.DeviceId;
        var devices = AudioHapticsEngine.GetRenderDevices();
        _audioHapticsUiLoading = true;
        AudioHapticsDeviceCombo.ItemsSource = devices;
        var selected = devices.FirstOrDefault(device =>
            string.Equals(device.Id, selectedId, StringComparison.OrdinalIgnoreCase))
            ?? devices.FirstOrDefault(device => device.IsDefault)
            ?? devices.FirstOrDefault();
        AudioHapticsDeviceCombo.SelectedItem = selected;
        _audioHapticsUiLoading = false;
        if (selected is not null && string.IsNullOrWhiteSpace(_audioHapticsSettings.DeviceId))
        {
            _audioHapticsSettings.DeviceId = selected.Id;
        }
    }

    private void RefreshAudioHapticsTargets()
    {
        if (AudioHapticsTargetCombo is null || _audioHaptics.IsRunning)
        {
            return;
        }

        var targets = new List<AudioHapticsTarget>
        {
            new("bridge", "", "虚拟桥接手柄（蓝牙）")
        };
        // A USBip-emulated "DualSense (USB)" is the virtual copy of a
        // Bluetooth bridge controller (detected by its PnP parent chain).
        // Writing the 4-channel audio to that virtual endpoint crackles
        // (real cable does the same over the USB audio endpoint), while the
        // bridge path (0x36/Opus) is clean, so keep only the bridge target.
        foreach (var row in Controllers.Where(row => row.IsUsb))
        {
            if (IsDualSenseUsb(row.Address) && row.IsVirtual)
            {
                continue;
            }
            var mode = IsDualSenseUsb(row.Address) ? "usb_audio" : "usb_rumble";
            targets.Add(new AudioHapticsTarget(mode, row.Address, row.Name));
        }
        // A DualShock 4 paired over Bluetooth has no Windows audio endpoint;
        // the speaker is driven by SBC audio inside HID reports. Offer it as
        // its own target whenever the device is reachable.
        var btDs4Path = AudioHapticsEngine.FindDs4BtHidPath();
        if (btDs4Path is not null)
        {
            targets.Add(new AudioHapticsTarget(
                "bt_sbc", btDs4Path, "DualShock 4（蓝牙扬声器）"));
        }

        var selected = targets.FirstOrDefault(target =>
                string.Equals(
                    target.Mode,
                    _audioHapticsSettings.TargetMode,
                    StringComparison.OrdinalIgnoreCase) &&
                string.Equals(
                    target.DeviceId,
                    _audioHapticsSettings.TargetDeviceId,
                    StringComparison.OrdinalIgnoreCase))
            ?? targets[0];
        _audioHapticsUiLoading = true;
        AudioHapticsTargetCombo.ItemsSource = targets;
        AudioHapticsTargetCombo.SelectedItem = selected;
        _audioHapticsUiLoading = false;
    }

    private static bool IsDualSenseUsb(string address) =>
        address.Contains("pid_0ce6", StringComparison.OrdinalIgnoreCase) ||
        address.Contains("pid_0df2", StringComparison.OrdinalIgnoreCase);

    private static void SelectComboItemByTag(ComboBox comboBox, string tag)
    {
        comboBox.SelectedItem = comboBox.Items
            .OfType<ComboBoxItem>()
            .FirstOrDefault(item => string.Equals(
                item.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
            ?? comboBox.Items.OfType<ComboBoxItem>().FirstOrDefault();
    }

    private AudioHapticsSettings BuildAudioHapticsSettingsFromUi()
    {
        var lowCut = Math.Round(AudioHapticsLowCutSlider.Value / 5) * 5;
        var highCut = Math.Round(AudioHapticsHighCutSlider.Value / 10) * 10;
        if (highCut <= lowCut + 10)
        {
            highCut = Math.Min(AudioHapticsHighCutSlider.Maximum, lowCut + 10);
        }
        return new AudioHapticsSettings
        {
            DeviceId = (AudioHapticsDeviceCombo.SelectedItem as AudioRenderDevice)?.Id ?? _audioHapticsSettings.DeviceId,
            TargetMode = (AudioHapticsTargetCombo.SelectedItem as AudioHapticsTarget)?.Mode ?? "bridge",
            TargetDeviceId = (AudioHapticsTargetCombo.SelectedItem as AudioHapticsTarget)?.DeviceId ?? "",
            AutoStart = AudioHapticsAutoStartCheckBox.IsChecked == true,
            StrengthPercent = Math.Round(AudioHapticsStrengthSlider.Value / 5) * 5,
            InputGainDb = Math.Round(AudioHapticsGainSlider.Value),
            LowCutHz = lowCut,
            HighCutHz = highCut,
            GateDb = Math.Round(AudioHapticsGateSlider.Value),
            CompressionRatio = Math.Round(AudioHapticsCompressionSlider.Value * 2) / 2,
            AttackMs = Math.Round(AudioHapticsAttackSlider.Value),
            ReleaseMs = Math.Round(AudioHapticsReleaseSlider.Value / 10) * 10,
            StereoWidthPercent = Math.Round(AudioHapticsWidthSlider.Value / 5) * 5,
            UsbLatencyMs = Math.Round(AudioHapticsUsbLatencySlider.Value),
            CaptureLatencyMs = Math.Round(AudioHapticsCaptureLatencySlider.Value),
            PlaybackQueueMs = Math.Round(AudioHapticsPlaybackQueueSlider.Value),
            LeftPercent = Math.Round(AudioHapticsLeftSlider.Value / 5) * 5,
            RightPercent = Math.Round(AudioHapticsRightSlider.Value / 5) * 5,
            CeilingPercent = Math.Round(AudioHapticsCeilingSlider.Value),
            ChannelMode = (AudioHapticsChannelModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "stereo",
            InvertRight = AudioHapticsInvertRightCheckBox.IsChecked == true,
            SpeakerEnabled = AudioHapticsSpeakerCheckBox.IsChecked == true,
            SpeakerVolumePercent = Math.Round(AudioHapticsSpeakerVolumeSlider.Value / 5) * 5,
            SpeakerPreamp = (int)Math.Round(AudioHapticsSpeakerPreampSlider.Value),
            HapticsGain = Math.Round(AudioHapticsVoiceCoilGainSlider.Value * 10) / 10,
            LeftMotorSource = ComboTagToInt(AudioHapticsLeftSourceCombo, 0),
            RightMotorSource = ComboTagToInt(AudioHapticsRightSourceCombo, 1),
            EqMode = (AudioHapticsEqModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "off",
            Eq3Band1GainDb = Math.Round(AudioHapticsEq3Band1Slider.Value),
            Eq3Band2GainDb = Math.Round(AudioHapticsEq3Band2Slider.Value),
            Eq3Band3GainDb = Math.Round(AudioHapticsEq3Band3Slider.Value),
            Eq6Band1GainDb = Math.Round(AudioHapticsEq6Band1Slider.Value),
            Eq6Band2GainDb = Math.Round(AudioHapticsEq6Band2Slider.Value),
            Eq6Band3GainDb = Math.Round(AudioHapticsEq6Band3Slider.Value),
            Eq6Band4GainDb = Math.Round(AudioHapticsEq6Band4Slider.Value),
            Eq6Band5GainDb = Math.Round(AudioHapticsEq6Band5Slider.Value),
            Eq6Band6GainDb = Math.Round(AudioHapticsEq6Band6Slider.Value)
        };
    }

    /// <summary>
    /// Marks bands above the 1.5 kHz haptics Nyquist, which cannot affect the
    /// voice coils because PcmAudioExtractor decimates 16:1 down to 3 kHz.
    /// </summary>
    private static string FormatEqBand(string name, double hz, double gainDb)
    {
        var label = hz >= 1000 ? $"{hz / 1000:0.0} kHz" : $"{hz:0} Hz";
        var inaudible = hz > 1500 ? "  · 对振动无效" : "";
        return $"{name}  {label}   {gainDb:+0;-0;0} dB{inaudible}";
    }

    private async void AudioHapticsMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_audioHapticsUiReady || _audioHapticsUiLoading)
        {
            return;
        }
        var native = (AudioHapticsModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() == "native";
        if (native == VdsBackend.IsNativeHaptics())
        {
            UpdateHapticsModeHint();
            return;
        }

        _busy = true;
        SetAudioHapticsStatus("正在切换触觉输出方式并重启服务...", NeutralForeground);
        try
        {
            await _backend.SetHapticsModeAsync(native);
            SetAudioHapticsStatus(
                native ? "已切换到原生音圈波形。" : "已切换到马达标量兼容模式。",
                SuccessForeground);
        }
        catch (Exception error)
        {
            SetAudioHapticsStatus($"切换失败：{SimplifyError(error)}", ErrorForeground);
        }
        finally
        {
            _busy = false;
            UpdateHapticsModeHint();
            await RefreshAsync(false);
        }
    }

    private void UpdateHapticsModeHint()
    {
        var native = VdsBackend.IsNativeHaptics();
        _audioHapticsUiLoading = true;
        SelectComboItemByTag(AudioHapticsModeCombo, native ? "native" : "rumble");
        _audioHapticsUiLoading = false;
        AudioHapticsModeHintText.Text = native
            ? "发送完整的 64 点音圈波形，细腻度等同 USB 直连。"
            : "把波形压成左右两个强度值交给手柄合成，兼容性更好但细节较少。";
    }

    private void AudioHapticsRestoreHeadroom_Click(object sender, RoutedEventArgs e)
    {
        _audioHapticsUiLoading = true;
        AudioHapticsStrengthSlider.Value = 100;
        AudioHapticsGainSlider.Value = 0;
        AudioHapticsLeftSlider.Value = 100;
        AudioHapticsRightSlider.Value = 100;
        AudioHapticsVoiceCoilGainSlider.Value = 1.0;
        AudioHapticsCeilingSlider.Value = 92;
        _audioHapticsUiLoading = false;
        ApplyAudioHapticsSettingsFromUi(true);
    }

    private static int ComboTagToInt(ComboBox comboBox, int fallback) =>
        int.TryParse((comboBox.SelectedItem as ComboBoxItem)?.Tag?.ToString(), out var value)
            ? value
            : fallback;

    private void ApplyAudioHapticsSettingsFromUi(bool markCustom)
    {
        if (!_audioHapticsUiReady || _audioHapticsUiLoading)
        {
            return;
        }
        _audioHapticsSettings = BuildAudioHapticsSettingsFromUi();
        _audioHaptics.UpdateSettings(_audioHapticsSettings);
        AudioHapticsStrengthText.Text = $"{_audioHapticsSettings.StrengthPercent:0}%";
        AudioHapticsGainText.Text = $"{_audioHapticsSettings.InputGainDb:+0;-0;0} dB";
        AudioHapticsLowCutText.Text = $"{_audioHapticsSettings.LowCutHz:0} Hz";
        AudioHapticsHighCutText.Text = $"{_audioHapticsSettings.HighCutHz:0} Hz";
        AudioHapticsWidthText.Text = $"{_audioHapticsSettings.StereoWidthPercent:0}%";
        AudioHapticsUsbLatencyText.Text = $"{_audioHapticsSettings.UsbLatencyMs:0} ms";
        AudioHapticsCaptureLatencyText.Text = $"{_audioHapticsSettings.CaptureLatencyMs:0} ms";
        AudioHapticsPlaybackQueueText.Text = $"{_audioHapticsSettings.PlaybackQueueMs:0} ms";
        AudioHapticsGateText.Text = $"{_audioHapticsSettings.GateDb:0} dB";
        AudioHapticsCompressionText.Text = $"{_audioHapticsSettings.CompressionRatio:0.0}:1";
        AudioHapticsAttackText.Text = $"{_audioHapticsSettings.AttackMs:0} ms";
        AudioHapticsReleaseText.Text = $"{_audioHapticsSettings.ReleaseMs:0} ms";
        AudioHapticsLeftText.Text = $"{_audioHapticsSettings.LeftPercent:0}%";
        AudioHapticsRightText.Text = $"{_audioHapticsSettings.RightPercent:0}%";
        AudioHapticsCeilingText.Text = $"{_audioHapticsSettings.CeilingPercent:0}%";
        AudioHapticsVoiceCoilGainText.Text = $"{_audioHapticsSettings.HapticsGain:0.0}x";
        AudioHapticsSpeakerPanel.Visibility = _audioHapticsSettings.SpeakerEnabled
            ? Visibility.Visible
            : Visibility.Collapsed;
        AudioHapticsSpeakerVolumeText.Text = _audioHapticsSettings.SpeakerEnabled
            ? $"{_audioHapticsSettings.SpeakerVolumePercent:0}%"
            : "";
        AudioHapticsSpeakerPreampText.Text = _audioHapticsSettings.SpeakerEnabled
            ? $"{_audioHapticsSettings.SpeakerPreamp:0}（2≈+6dB）"
            : "";
        AudioHapticsHeadroomPanel.Visibility = _audioHapticsSettings.ExceedsSafeHeadroom
            ? Visibility.Visible
            : Visibility.Collapsed;
        AudioHapticsEq3Panel.Visibility = _audioHapticsSettings.EqMode == "band3"
            ? Visibility.Visible
            : Visibility.Collapsed;
        AudioHapticsEq6Panel.Visibility = _audioHapticsSettings.EqMode == "band6"
            ? Visibility.Visible
            : Visibility.Collapsed;
        AudioHapticsEq3Band1Text.Text = FormatEqBand("低架", _audioHapticsSettings.Eq3Band1Hz, _audioHapticsSettings.Eq3Band1GainDb);
        AudioHapticsEq3Band2Text.Text = FormatEqBand("中频", _audioHapticsSettings.Eq3Band2Hz, _audioHapticsSettings.Eq3Band2GainDb);
        AudioHapticsEq3Band3Text.Text = FormatEqBand("高架", _audioHapticsSettings.Eq3Band3Hz, _audioHapticsSettings.Eq3Band3GainDb);
        AudioHapticsEq6Band1Text.Text = FormatEqBand("超低", _audioHapticsSettings.Eq6Band1Hz, _audioHapticsSettings.Eq6Band1GainDb);
        AudioHapticsEq6Band2Text.Text = FormatEqBand("低频", _audioHapticsSettings.Eq6Band2Hz, _audioHapticsSettings.Eq6Band2GainDb);
        AudioHapticsEq6Band3Text.Text = FormatEqBand("中低", _audioHapticsSettings.Eq6Band3Hz, _audioHapticsSettings.Eq6Band3GainDb);
        AudioHapticsEq6Band4Text.Text = FormatEqBand("中频", _audioHapticsSettings.Eq6Band4Hz, _audioHapticsSettings.Eq6Band4GainDb);
        AudioHapticsEq6Band5Text.Text = FormatEqBand("中高", _audioHapticsSettings.Eq6Band5Hz, _audioHapticsSettings.Eq6Band5GainDb);
        AudioHapticsEq6Band6Text.Text = FormatEqBand("高频", _audioHapticsSettings.Eq6Band6Hz, _audioHapticsSettings.Eq6Band6GainDb);
        if (markCustom)
        {
            _audioHapticsUiLoading = true;
            SelectComboItemByTag(AudioHapticsPresetCombo, "custom");
            _audioHapticsUiLoading = false;
        }
        _audioHapticsSettingsSaveTimer.Stop();
        _audioHapticsSettingsSaveTimer.Start();
    }

    private void SaveAudioHapticsSettings()
    {
        _audioHapticsSettingsSaveTimer.Stop();
        try
        {
            _audioHapticsSettings.Save();
        }
        catch (Exception error)
        {
            SetAudioHapticsStatus($"无法保存音频触觉设置：{SimplifyError(error)}", ErrorForeground);
        }
    }

    private async Task StartAudioHapticsAsync(bool showErrors)
    {
        if (_audioHaptics.IsRunning)
        {
            return;
        }
        var targetMode = (AudioHapticsTargetCombo.SelectedItem as AudioHapticsTarget)?.Mode
            ?? "bridge";
        var needsService = targetMode == "bridge";
        if (needsService &&
            (_snapshot?.ServiceState != VdsServiceState.Running ||
             _snapshot.UpdateAvailable == true))
        {
            if (showErrors)
            {
                ShowError(new InvalidOperationException("请先安装新版核心并启动 vDS 服务。"));
            }
            return;
        }
        ApplyAudioHapticsSettingsFromUi(false);
        AudioHapticsToggleButton.IsEnabled = false;
        try
        {
            await _audioHaptics.StartAsync(_audioHapticsSettings);
            SetAudioHapticsStatus("正在捕获桌面音频，调整滑条会实时生效。", SuccessForeground);
            SetActivity("桌面音频触觉已启用，正在驱动手柄音圈马达。", SuccessForeground);
        }
        catch (Exception error)
        {
            SetAudioHapticsStatus(SimplifyError(error), ErrorForeground);
            if (showErrors)
            {
                ShowError(error);
            }
        }
        finally
        {
            UpdateAudioHapticsAvailability();
        }
    }

    private async Task StopAudioHapticsAsync()
    {
        AudioHapticsToggleButton.IsEnabled = false;
        await _audioHaptics.StopAsync();
        AudioHapticsInputMeter.Value = 0;
        AudioHapticsOutputMeter.Value = 0;
        SetAudioHapticsStatus("音频触觉已停止。", NeutralForeground);
        UpdateAudioHapticsAvailability();
    }

    private void SetAudioHapticsStatus(string text, Brush foreground)
    {
        AudioHapticsStatusText.Text = text;
        AudioHapticsStatusText.Foreground = foreground;
    }

    private void AudioHaptics_LevelsChanged(object? sender, AudioHapticsLevels levels)
    {
        Dispatcher.BeginInvoke(() =>
        {
            AudioHapticsInputMeter.Value = Math.Clamp(Math.Max(levels.InputLeft, levels.InputRight), 0, 1);
            AudioHapticsOutputMeter.Value = Math.Clamp(Math.Max(levels.OutputLeft, levels.OutputRight), 0, 1);
        });
    }

    private void AudioHaptics_Faulted(object? sender, string message)
    {
        Dispatcher.BeginInvoke(() => SetAudioHapticsStatus(message, ErrorForeground));
    }

    private async void AudioHapticsToggle_Click(object sender, RoutedEventArgs e)
    {
        if (_audioHaptics.IsRunning)
        {
            await StopAudioHapticsAsync();
        }
        else
        {
            await StartAudioHapticsAsync(true);
        }
    }

    private void AudioHapticsRefreshDevices_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            RefreshAudioHapticsDevices();
            RefreshAudioHapticsTargets();
            ApplyAudioHapticsSettingsFromUi(false);
            SetAudioHapticsStatus("播放设备列表已刷新。", NeutralForeground);
        }
        catch (Exception error)
        {
            SetAudioHapticsStatus(SimplifyError(error), ErrorForeground);
        }
    }

    private static bool CaptureSharesController(
        AudioRenderDevice? capture, AudioHapticsTarget? target)
    {
        if (capture is null || target is null ||
            string.IsNullOrWhiteSpace(target.DeviceId) ||
            string.IsNullOrWhiteSpace(capture.ControllerDeviceId))
        {
            return false;
        }
        var match = Regex.Match(
            target.DeviceId,
            @"vid_([0-9a-f]{4})&pid_([0-9a-f]{4})",
            RegexOptions.IgnoreCase);
        return match.Success &&
               capture.ControllerDeviceId.Contains(
                   $"VID_{match.Groups[1].Value}&PID_{match.Groups[2].Value}",
                   StringComparison.OrdinalIgnoreCase);
    }

    private void UpdateAudioHapticsSpeakerGuard()
    {
        if (_audioHapticsUiLoading || AudioHapticsSpeakerCheckBox is null)
        {
            return;
        }
        var capture = AudioHapticsDeviceCombo.SelectedItem as AudioRenderDevice;
        var target = AudioHapticsTargetCombo.SelectedItem as AudioHapticsTarget;
        if (CaptureSharesController(capture, target))
        {
            if (AudioHapticsSpeakerCheckBox.IsEnabled)
            {
                _speakerEnabledBeforeControllerCapture =
                    AudioHapticsSpeakerCheckBox.IsChecked == true;
            }
            AudioHapticsSpeakerCheckBox.IsChecked = false;
            AudioHapticsSpeakerCheckBox.IsEnabled = false;
            SetAudioHapticsStatus(
                "采集源与输出目标是同一个手柄：扬声器转发已自动关闭（避免反馈啸叫），手柄硬件音量已保留，仅驱动震动。",
                WarningForeground);
            return;
        }
        var wasBlocked = !AudioHapticsSpeakerCheckBox.IsEnabled;
        AudioHapticsSpeakerCheckBox.IsEnabled = true;
        if (wasBlocked)
        {
            AudioHapticsSpeakerCheckBox.IsChecked =
                _speakerEnabledBeforeControllerCapture;
        }
    }

    private void AudioHapticsDevice_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateAudioHapticsSpeakerGuard();
        ApplyAudioHapticsSettingsFromUi(false);
        if (_audioHaptics.IsRunning)
        {
            SetAudioHapticsStatus("捕获设备已更改，停止后重新开始即可切换。", WarningForeground);
        }
    }

    private void AudioHapticsTarget_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_audioHapticsUiLoading)
        {
            return;
        }
        UpdateAudioHapticsSpeakerGuard();
        ApplyAudioHapticsSettingsFromUi(false);
        UpdateAudioHapticsAvailability();
        if (_audioHaptics.IsRunning)
        {
            SetAudioHapticsStatus("手柄目标已更改，停止后重新开始即可切换。", WarningForeground);
        }
    }

    private void AudioHapticsAutoStart_Changed(object sender, RoutedEventArgs e) =>
        ApplyAudioHapticsSettingsFromUi(false);

    private void AudioHapticsSetting_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        ApplyAudioHapticsSettingsFromUi(true);

    private void AudioHapticsSetting_ValueChanged(object sender, SelectionChangedEventArgs e) =>
        ApplyAudioHapticsSettingsFromUi(true);

    private void AudioHapticsSetting_ValueChanged(object sender, RoutedEventArgs e) =>
        ApplyAudioHapticsSettingsFromUi(true);

    private void AudioHapticsPreset_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_audioHapticsUiLoading || AudioHapticsPresetCombo.SelectedItem is not ComboBoxItem item)
        {
            return;
        }
        _audioHapticsUiLoading = true;
        switch (item.Tag?.ToString())
        {
            case "bass":
                ApplyAudioHapticsPreset(130, 2, 22, 220, -62, 3.5, 5, 150, 105, 94);
                break;
            case "detail":
                ApplyAudioHapticsPreset(92, 4, 55, 720, -54, 2, 2, 90, 130, 88);
                break;
            case "cinema":
                ApplyAudioHapticsPreset(118, 1, 25, 420, -64, 4.5, 9, 240, 115, 92);
                break;
            case "balanced":
                ApplyAudioHapticsPreset(100, 0, 35, 320, -58, 2.5, 4, 120, 100, 92);
                break;
        }
        _audioHapticsUiLoading = false;
        ApplyAudioHapticsSettingsFromUi(false);
    }

    private void ApplyAudioHapticsPreset(
        double strength,
        double gain,
        double lowCut,
        double highCut,
        double gate,
        double compression,
        double attack,
        double release,
        double width,
        double ceiling)
    {
        AudioHapticsStrengthSlider.Value = strength;
        AudioHapticsGainSlider.Value = gain;
        AudioHapticsLowCutSlider.Value = lowCut;
        AudioHapticsHighCutSlider.Value = highCut;
        AudioHapticsGateSlider.Value = gate;
        AudioHapticsCompressionSlider.Value = compression;
        AudioHapticsAttackSlider.Value = attack;
        AudioHapticsReleaseSlider.Value = release;
        AudioHapticsWidthSlider.Value = width;
        AudioHapticsCeilingSlider.Value = ceiling;

        // A preset must define the whole voice-coil chain. Without this the
        // per-motor trim and the EQ/gain mix settings survive a preset change
        // and silently colour it.
        AudioHapticsLeftSlider.Value = 100;
        AudioHapticsRightSlider.Value = 100;
        AudioHapticsVoiceCoilGainSlider.Value = 1.0;
        AudioHapticsInvertRightCheckBox.IsChecked = false;
        SelectComboItemByTag(AudioHapticsChannelModeCombo, "stereo");
        SelectComboItemByTag(AudioHapticsLeftSourceCombo, "0");
        SelectComboItemByTag(AudioHapticsRightSourceCombo, "1");
        SelectComboItemByTag(AudioHapticsEqModeCombo, "off");
        foreach (var slider in new[]
                 {
                     AudioHapticsEq3Band1Slider, AudioHapticsEq3Band2Slider, AudioHapticsEq3Band3Slider,
                     AudioHapticsEq6Band1Slider, AudioHapticsEq6Band2Slider, AudioHapticsEq6Band3Slider,
                     AudioHapticsEq6Band4Slider, AudioHapticsEq6Band5Slider, AudioHapticsEq6Band6Slider
                 })
        {
            slider.Value = 0;
        }
    }

    private void RunShellAction(Action action)
    {
        try
        {
            action();
        }
        catch (Exception error)
        {
            ShowError(error);
        }
    }

    private static Brush BrushFrom(string color) =>
        new BrushConverter().ConvertFromString(color) as Brush ?? Brushes.Gray;
}
