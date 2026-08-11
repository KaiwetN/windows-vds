# Windows 手柄输出测试例程

`test-controller-output.ps1` 会直接测试 vDS 的 Windows 输出链路，不需要游戏或
DSX 客户端。它使用已有的两条本机接口：DSX UDP 接口临时设置灯光和自适应扳机，
`\\.\pipe\vdsd-audio` 注入四通道 PCM 音频。后者的通道顺序是：

| PCM 通道 | 测试项目 | 经过的 vDS 输出 |
| --- | --- | --- |
| 0、1 | 手柄扬声器或耳机的 880 Hz 测试音 | USB 音频到蓝牙 `0x36` 音频包 |
| 2、3 | 左 70 Hz、右 180 Hz 音圈波形 | 原生 DualSense 触觉波形 |
| 2、3 | 左 4 Hz、右 3 Hz 的脉冲包络 | 标准马达兼容输出或原生音圈脉冲 |

## 准备

1. 使用当前源码构建并安装 `vdsd`，再启动 `vdsd` 服务。
2. 通过 GUI 或 `connect-controller.ps1` 桥接手柄；手柄必须已连接且不是休眠状态。
3. 暂停游戏、DSX，以及控制中心中的「桌面音频触觉」。音频注入管道同一时刻只接受
   一个客户端。
4. 若要验证真实的音圈波形，设置原生触觉模式并重启服务：

```powershell
Set-Content "$env:ProgramData\vDS\haptics-mode.txt" native -NoNewline
Restart-Service vdsd
```

默认 `rumble` 模式会把每个音圈窗口的能量转换为传统左右马达强度；它能验证兼容
输出，但不能验证原始波形是否完整抵达手柄。

## 运行

从仓库根目录执行完整测试：

```powershell
.\test-controller-output.ps1
```

完整测试每个阶段默认持续两秒，顺序为：红/绿/蓝灯光，两个自适应扳机效果，
扬声器测试音，双音圈波形，以及左右不同频率的马达脉冲。可按单项执行或缩短阶段：

```powershell
.\test-controller-output.ps1 -Test VoiceCoils -StageSeconds 3 -RequireNativeHaptics
.\test-controller-output.ps1 -Test Triggers
.\test-controller-output.ps1 -Test Speaker
```

脚本开始前会读取并保存当前 vDS 灯效和扳机配置；正常退出或出错时都会恢复这些
配置。DSX API 默认监听 `127.0.0.1:6969`；若 `dsx-api.txt` 配置了其他端口，传入
`-DsxPort <端口>`。端口被 DSX 本体占用或 API 被关闭时，灯光和扳机阶段不会达到
手柄，即使音频阶段仍可单独运行。

## 判定

| 可观察结果 | 说明 |
| --- | --- |
| 三次灯光颜色、玩家灯和麦克风灯依次变化 | 控制管道、效果仲裁和蓝牙状态报告正常 |
| L2 阻力/振动，R2 段落/连发阻力 | 虚拟 USB 输出中的自适应扳机路径正常 |
| 手柄扬声器或其耳机口有 880 Hz 测试音 | 虚拟 USB 音频、Opus 编码和蓝牙音频输出正常 |
| 原生模式下感到左右不同的连续音圈纹理 | `0x36` 原始触觉波形链路正常 |
| `rumble` 模式下左右有不同节奏的脉冲 | 原生波形到标准马达兼容回退正常 |

全部阶段都可观察到反馈，说明 vDS 已覆盖有线 DualSense 常用的输出能力。它不验证
游戏是否选择了正确的音频端点，也不验证麦克风采集；这两项需要在目标游戏中单独测试。
