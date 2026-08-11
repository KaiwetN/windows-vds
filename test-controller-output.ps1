# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [ValidateSet("All", "Lights", "Triggers", "Speaker", "VoiceCoils", "Rumble")]
  [string]$Test = "All",
  [ValidateRange(1, 10)]
  [int]$StageSeconds = 2,
  [ValidateRange(1, 65535)]
  [int]$DsxPort = 6969,
  [switch]$RequireNativeHaptics
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$audioInjectorSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Threading;

namespace VdsControllerOutputTest
{
    public enum AudioStage
    {
        Silence,
        SpeakerTone,
        VoiceCoils,
        MotorEnvelope
    }

    public sealed class AudioInjector : IDisposable
    {
        private const int SampleRate = 48000;
        private const int FramesPerChunk = 512;
        private const int ChannelsPerFrame = 4;
        private const int BytesPerSample = 2;
        private const int BytesPerChunk = FramesPerChunk * ChannelsPerFrame * BytesPerSample;
        private NamedPipeClientStream stream;
        private long framePosition;

        public void Connect()
        {
            stream = new NamedPipeClientStream(
                ".", "vdsd-audio", PipeDirection.Out, PipeOptions.WriteThrough);
            stream.Connect(2500);

            var header = new byte[16];
            WriteUInt32(header, 0, 0x41534456);
            WriteUInt16(header, 4, 2);
            WriteUInt16(header, 6, ChannelsPerFrame);
            WriteUInt32(header, 8, SampleRate);
            WriteUInt32(header, 12, FramesPerChunk);
            stream.Write(header, 0, header.Length);
            stream.Flush();
        }

        public void Play(AudioStage stage, int durationMilliseconds)
        {
            if (stream == null)
            {
                throw new InvalidOperationException("Audio injector is not connected.");
            }

            var chunks = Math.Max(1, (int)Math.Ceiling(
                durationMilliseconds * (double)SampleRate / FramesPerChunk / 1000));
            var clock = Stopwatch.StartNew();
            for (var chunk = 0; chunk < chunks; ++chunk)
            {
                var pcm = new byte[BytesPerChunk];
                FillChunk(pcm, stage);
                stream.Write(pcm, 0, pcm.Length);

                var targetMilliseconds = (chunk + 1) * FramesPerChunk * 1000.0 / SampleRate;
                var remainingMilliseconds = targetMilliseconds - clock.Elapsed.TotalMilliseconds;
                if (remainingMilliseconds > 1)
                {
                    Thread.Sleep((int)remainingMilliseconds);
                }
                else
                {
                    Thread.Yield();
                }
            }
            stream.Flush();
        }

        public void Dispose()
        {
            if (stream != null)
            {
                stream.Dispose();
                stream = null;
            }
        }

        private void FillChunk(byte[] pcm, AudioStage stage)
        {
            for (var frame = 0; frame < FramesPerChunk; ++frame)
            {
                var time = framePosition / (double)SampleRate;
                short speakerLeft = 0;
                short speakerRight = 0;
                short hapticsLeft = 0;
                short hapticsRight = 0;

                switch (stage)
                {
                    case AudioStage.SpeakerTone:
                        speakerLeft = Sine(880, 11000, time);
                        speakerRight = Sine(880, 11000, time);
                        break;
                    case AudioStage.VoiceCoils:
                        hapticsLeft = Sine(70, 20000, time);
                        hapticsRight = Sine(180, 20000, time);
                        break;
                    case AudioStage.MotorEnvelope:
                        hapticsLeft = Scale(Sine(105, 24000, time), Pulse(4, time));
                        hapticsRight = Scale(Sine(145, 24000, time), Pulse(3, time + .17));
                        break;
                }

                var offset = frame * ChannelsPerFrame * BytesPerSample;
                WriteInt16(pcm, offset + 0, speakerLeft);
                WriteInt16(pcm, offset + 2, speakerRight);
                WriteInt16(pcm, offset + 4, hapticsLeft);
                WriteInt16(pcm, offset + 6, hapticsRight);
                ++framePosition;
            }
        }

        private static short Sine(double frequency, double amplitude, double time)
        {
            return (short)Math.Round(Math.Sin(2 * Math.PI * frequency * time) * amplitude);
        }

        private static short Scale(short sample, double multiplier)
        {
            return (short)Math.Round(sample * multiplier);
        }

        private static double Pulse(double frequency, double time)
        {
            return .15 + .85 * Math.Max(0, Math.Sin(2 * Math.PI * frequency * time));
        }

        private static void WriteInt16(byte[] destination, int offset, short value)
        {
            destination[offset] = unchecked((byte)value);
            destination[offset + 1] = unchecked((byte)(value >> 8));
        }

        private static void WriteUInt16(byte[] destination, int offset, int value)
        {
            destination[offset] = unchecked((byte)value);
            destination[offset + 1] = unchecked((byte)(value >> 8));
        }

        private static void WriteUInt32(byte[] destination, int offset, uint value)
        {
            destination[offset] = unchecked((byte)value);
            destination[offset + 1] = unchecked((byte)(value >> 8));
            destination[offset + 2] = unchecked((byte)(value >> 16));
            destination[offset + 3] = unchecked((byte)(value >> 24));
        }
    }
}
'@

if ($null -eq ("VdsControllerOutputTest.AudioInjector" -as [type])) {
  Add-Type -TypeDefinition $audioInjectorSource -Language CSharp
}

function Invoke-VdsControlCommand {
  param([Parameter(Mandatory = $true)][hashtable]$Command)

  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".", "vdsd", [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)
  try {
    $pipe.Connect(2500)
    $payload = [System.Text.Encoding]::UTF8.GetBytes(
      (($Command | ConvertTo-Json -Compress -Depth 4) + "`n"))
    $pipe.Write($payload, 0, $payload.Length)
    $pipe.Flush()

    $reply = [System.Text.StringBuilder]::new()
    $buffer = [byte[]]::new(1024)
    do {
      $count = $pipe.Read($buffer, 0, $buffer.Length)
      if ($count -eq 0) {
        throw "vDS 控制管道在返回应答前关闭。"
      }
      [void]$reply.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $count))
    } while ($reply.ToString().IndexOf("`n") -lt 0)

    return $reply.ToString() | ConvertFrom-Json
  }
  finally {
    $pipe.Dispose()
  }
}

function Send-DsxInstructions {
  param([Parameter(Mandatory = $true)][object[]]$Instructions)

  $udp = [System.Net.Sockets.UdpClient]::new()
  try {
    $udp.Connect("127.0.0.1", $DsxPort)
    $payload = @{ instructions = $Instructions } | ConvertTo-Json -Compress -Depth 4
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
    [void]$udp.Send($bytes, $bytes.Length)
  }
  finally {
    $udp.Dispose()
  }
}

function Start-LightSequence {
  foreach ($stage in @(
      @{ Name = "红色"; Red = 255; Green = 0; Blue = 0 },
      @{ Name = "绿色"; Red = 0; Green = 255; Blue = 0 },
      @{ Name = "蓝色"; Red = 0; Green = 80; Blue = 255 })) {
    Write-Host "[灯光] $($stage.Name)"
    Send-DsxInstructions @(
      @{ type = 2; parameters = @(0, $stage.Red, $stage.Green, $stage.Blue) },
      @{ type = 3; parameters = @(0, 3) },
      @{ type = 5; parameters = @(0, 1) })
    Start-Sleep -Seconds $StageSeconds
  }
}

function Start-TriggerSequence {
  Write-Host "[自适应扳机] L2 阻力，R2 半自动扳机"
  Send-DsxInstructions @(
    @{ type = 1; parameters = @(0, 1, 13, 2, 7) },
    @{ type = 1; parameters = @(0, 2, 16, 2, 6, 8) })
  Start-Sleep -Seconds $StageSeconds

  Write-Host "[自适应扳机] L2 振动，R2 自动扳机"
  Send-DsxInstructions @(
    @{ type = 1; parameters = @(0, 1, 8, 36) },
    @{ type = 1; parameters = @(0, 2, 17, 1, 7, 22) })
  Start-Sleep -Seconds $StageSeconds
}

function Restore-Effects {
  param([Parameter(Mandatory = $true)]$Effects)

  $command = @{
    command = "effects"
    left_trigger = if ([string]::IsNullOrEmpty($Effects.left_trigger)) {
      "none"
    } else {
      "raw:$($Effects.left_trigger)"
    }
    right_trigger = if ([string]::IsNullOrEmpty($Effects.right_trigger)) {
      "none"
    } else {
      "raw:$($Effects.right_trigger)"
    }
    led = if ([string]::IsNullOrEmpty($Effects.led)) { "none" } else { $Effects.led }
    player = if ([string]::IsNullOrEmpty($Effects.player)) { "none" } else { $Effects.player }
    mute_led = if ([string]::IsNullOrEmpty($Effects.mute_led)) {
      "none"
    } else {
      $Effects.mute_led
    }
    force = $Effects.force
  }
  $response = Invoke-VdsControlCommand $command
  if (!$response.OK) {
    throw "恢复效果失败: $($response.error)"
  }
}

function Test-Includes {
  param([Parameter(Mandatory = $true)][string]$Name)
  return $Test -eq "All" -or $Test -eq $Name
}

$hapticsModePath = Join-Path $env:ProgramData "vDS\haptics-mode.txt"
$hapticsMode = if (Test-Path -LiteralPath $hapticsModePath -PathType Leaf) {
  (Get-Content -LiteralPath $hapticsModePath -Raw).Trim()
} else {
  "rumble"
}

if ($hapticsMode -ne "native") {
  $warning = "当前触觉模式是 '$hapticsMode'：音圈波形会被降级为标准马达强度。"
  if ($RequireNativeHaptics) {
    throw "$warning 请写入 '$hapticsModePath' 的 native 并重启 vdsd 服务后再运行。"
  }
  Write-Warning $warning
}

$previousEffects = $null
$injector = $null
try {
  $effectsReply = Invoke-VdsControlCommand @{ command = "effects" }
  if (!$effectsReply.OK) {
    throw "无法查询现有效果: $($effectsReply.error)"
  }
  $previousEffects = $effectsReply.effects

  if (Test-Includes "Lights") {
    Start-LightSequence
  }
  if (Test-Includes "Triggers") {
    Start-TriggerSequence
  }

  $audioTests = @("Speaker", "VoiceCoils", "Rumble") | Where-Object { Test-Includes $_ }
  if ($audioTests.Count -gt 0) {
    $injector = [VdsControllerOutputTest.AudioInjector]::new()
    try {
      $injector.Connect()
    }
    catch {
      throw "无法连接 \\.\pipe\vdsd-audio。请确认 vdsd 正在运行、手柄已桥接，并先停止控制中心的桌面音频触觉。$($_.Exception.Message)"
    }

    if (Test-Includes "Speaker") {
      Write-Host "[手柄扬声器/耳机] 880 Hz 测试音"
      $injector.Play([VdsControllerOutputTest.AudioStage]::SpeakerTone, $StageSeconds * 1000)
    }
    if (Test-Includes "VoiceCoils") {
      Write-Host "[音圈] 左 70 Hz，右 180 Hz 波形"
      $injector.Play([VdsControllerOutputTest.AudioStage]::VoiceCoils, $StageSeconds * 1000)
    }
    if (Test-Includes "Rumble") {
      Write-Host "[马达包络] 左 4 Hz，右 3 Hz 脉冲"
      $injector.Play([VdsControllerOutputTest.AudioStage]::MotorEnvelope, $StageSeconds * 1000)
    }
    $injector.Play([VdsControllerOutputTest.AudioStage]::Silence, 150)
  }

  Write-Host "测试序列已完成。请按文档核对每个阶段的实体反馈。"
}
finally {
  if ($null -ne $injector) {
    $injector.Dispose()
  }
  if ($null -ne $previousEffects) {
    try {
      Restore-Effects $previousEffects
      Write-Host "已恢复运行测试前的 vDS 灯效与扳机配置。"
    }
    catch {
      Write-Warning "无法自动恢复原有效果: $($_.Exception.Message)"
    }
  }
}
