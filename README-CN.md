# vDS — 虚拟 DualSense 手柄桥

[English](README.md) ｜ [Linux 详细指南](README-LINUX.md) ｜ [Windows 详细指南](README-WINDOWS.md)

vDS（virtual DualSense）是一个把实体 DualSense / DualSense Edge 手柄通过蓝牙
接入电脑，并向系统暴露成**完整虚拟 USB 手柄**的桥接程序。游戏看到的是
Sony USB 设备，而不是普通的 XInput 映射。

它支持按键、触摸板、陀螺仪、灯效、普通震动、自适应扳机、四声道触觉音频、
手柄扬声器 / 耳机输出和麦克风。固件更新不能通过虚拟设备进行。

在 Linux 和 Windows 上，`vdsd` 守护进程都直接通过蓝牙 HID 与实体手柄通信，
再将蓝牙协议转换成虚拟 USB 流量：

- **Linux**：通过内置内核模块 `vds_hcd.ko` 暴露虚拟 DualSense 设备；
- **Windows**：通过 usbip-win2 导出虚拟设备，并用 HidHide 隐藏实体蓝牙手柄，
  避免游戏收到两份输入。

详细的 DualSense 输出与触觉数据包处理基于
[DS5Dongle](https://github.com/awalol/DS5Dongle) 与协议抓包研究成果。
本仓库基于 MIT 许可的 [hurryman2212/vds](https://github.com/hurryman2212/vds)
`0.4.0-rc1`，来源记录见 `UPSTREAM.md`。

## 功能特性

- 完整的 USB DualSense 仿真：按键、触摸板、陀螺仪、灯条与玩家指示灯；
- 自适应扳机、普通震动和麦克风输入；
- 四声道触觉音频：左右音圈 + 扬声器/耳机的游戏音频通道；
- 蓝牙手柄扬声器 / 3.5mm 耳机输出（Opus 音频块）；
- Windows「桌面音频触觉」：捕获任意播放设备的系统混音驱动音圈；
- 与 DSX 公开 UDP 协议兼容的 API，现有游戏 mod 无需修改即可使用；
- 命令行 `vdsctl` 与 Windows 中文图形控制中心 `VdsGui`。

## 工作原理

### Windows 数据流

```text
应用程序
  -(WASAPI 渲染流)-> 音频引擎 [audiodg.exe / audioeng.dll]
  -(USB 音频 PCM)-> USB 音频类驱动 [Usbaudio.sys]
  -(USB 同步 OUT URB)-> Windows USB 栈 [usbip-win2]
  -(本地 TCP 栈)-> vdsd
  -(蓝牙 HID 输出报告)-> HIDClass + HidBth 传输小驱动
  -(蓝牙 HID Control/Interrupt)-> DualSense (Edge) 手柄
```

### Linux 数据流

```text
应用程序
  -> 用户态音频服务（如 PipeWire）
  -(ALSA PCM)-> Linux ALSA 栈 [snd-usb-audio.ko]
  -(USB 同步 OUT URB)-> Linux USB 栈 [vds_hcd.ko]
  -(/dev/vdsX)-> vdsd
  -(AF_BLUETOOTH L2CAP socket)-> Linux 蓝牙栈
  -(蓝牙 HID Control/Interrupt)-> DualSense (Edge) 手柄
```

## 平台与要求

### Windows

- 64 位 Windows 10 1903 或更新版本；
- 推荐使用 Intel BE200：其 Windows 蓝牙驱动能把 DualSense 暴露为蓝牙 HID
  设备，vDS 直接读写该设备，无需自定义蓝牙固件或 ESP32-S3 / Pico 2 W；
- 建议先通过 Windows Update 或 Intel Driver & Support Assistant 更新蓝牙驱动。

完整步骤见 [README-WINDOWS.md](README-WINDOWS.md)。

### Linux

- 需要与运行内核匹配的 Linux 内核头文件、`dkms` 和 C++20 工具链；
- 需要编译并加载自定义内核模块 `vds_hcd`（因为 `dummy_hcd` 不支持触觉音频
  所需的同步传输）；
- 当前限制：运行 `bluetoothd` 时需要禁用 input 插件
  （`--noplugin=input`），vDS 才能独占蓝牙 HID 通道。

完整步骤见 [README-LINUX.md](README-LINUX.md)。

## Windows 快速开始

### 编译

在普通 PowerShell 中运行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\build-windows.ps1
```

脚本会自动使用 Visual Studio 2022、其内置 CMake 和 vcpkg，生成 x64 Release
版本，同时生成中文 GUI：

```text
out\gui\VdsGui.exe
```

### 安装

双击 GUI 会请求管理员权限。它可以安装/更新 vDS、显示蓝牙与虚拟 USB 状态、
注册或取消注册手柄、打开 Windows 手柄测试以及查看服务日志。也可以使用
命令行方式，在「以管理员身份运行」的 PowerShell 中执行：

```powershell
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\setup-windows.ps1 -Mode Check
.\setup-windows.ps1 -Mode Install
```

安装模式会从官方 GitHub Release 下载并验证签名，然后安装：

- `usbip-win2`：把本机 USB/IP 端点导入为 Windows USB 设备；
- `HidHide`：隐藏实体蓝牙 HID，避免游戏收到两份输入；
- `vdsd`：自动启动的桥接服务；
- `vdsctl`：手柄注册和诊断工具；
- `VdsGui`：中文图形控制中心，并添加到开始菜单的 `vDS` 文件夹。

> **提示：** `VdsGui` 是框架依赖的 .NET 8 应用，需要
> [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)。
> 未安装时首次启动会提示缺少运行时；可提前用
> `winget install Microsoft.DotNet.DesktopRuntime.8` 安装。

> **注意：** 安装 USB/IP 驱动会重启 USB 3 Hub，键鼠等 USB 设备可能短暂断开。
> 如果脚本提示需要重启，请先重启 Windows。

### 配对并连接

1. 关闭可能独占手柄的 DS4Windows；首次验证时也建议暂时退出 Steam。
2. 在手柄关机时同时按住 **Create（触摸板左上方）+ PS**，直到灯条快速闪烁。
3. 打开 Windows「设置 → 蓝牙和设备 → 添加设备 → 蓝牙」，选择
   **Wireless Controller**。
4. 配对完成后按一次 PS 键，让手柄处于在线状态。
5. 在普通 PowerShell 中运行：

```powershell
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\connect-controller.ps1
```

只有一个已配对手柄时脚本会自动选择它，并自动分配空闲虚拟 USB 端口。多个
手柄可指定蓝牙地址；仅在需要固定端口时再传入 `-Port`：

```powershell
.\connect-controller.ps1 -Address "aa:bb:cc:dd:ee:ff" -Port 0
```

DualSense Edge 会被自动识别；也可通过 `-Profile ds5` 强制把它作为普通
DualSense 暴露，以兼容不认识 Edge 的游戏。

手柄注册信息保存在 `%ProgramData%\vDS\vdsd.db`。以后只需按 PS 键，后台服务
会自动恢复虚拟 USB 手柄，不必重复运行连接脚本。

## Linux 快速开始

### 安装依赖

Debian 系：

```sh
sudo apt install git build-essential dkms cmake pkg-config
sudo apt install libopus-dev libudev-dev libdbus-1-dev libbluetooth-dev
```

Arch / CachyOS 系：

```sh
sudo pacman -S git base-devel dkms cmake pkgconf
sudo pacman -S opus systemd dbus bluez-libs
```

另外安装与运行内核匹配的头文件包；若内核使用 LLVM 工具链构建，请安装对应
的 LLVM 工具链。

### 编译并安装内核模块

```sh
make -C module
sudo make -C module install
sudo modprobe vds_hcd
```

默认创建 4 个虚拟手柄端口，可用 `max_port` 覆盖（范围 1..4）：

```sh
sudo modprobe vds_hcd max_port=2
```

卸载模块：`sudo make -C module uninstall`。

### 编译并安装用户态工具

```sh
cmake . -B build
make -C build
sudo make -C build install
```

安装的文件为 `/usr/local/bin/vdsd` 和 `/usr/local/bin/vdsctl`。需要随系统
自动启动服务时，用 `INSTALL_SERVICE=YES` 重新配置：

```sh
cmake . -B build -DINSTALL_SERVICE=YES
make -C build
sudo make -C build install
sudo systemctl restart vdsd.service
```

### 配对实体手柄

> [!IMPORTANT]
>
> 当前限制：需要以 `--noplugin=input` 运行 `bluetoothd`，否则 BlueZ input
> 插件会先占用手柄并把它暴露成普通蓝牙手柄。辅助脚本
> `override-bluetoothd.sh` 可以安装/移除所需的 drop-in 覆盖：
>
> ```sh
> sudo ./override-bluetoothd.sh disable-input --restart
> sudo ./override-bluetoothd.sh enable-input --restart
> ```

用 `bluetoothctl` 配对：让手柄进入配对模式（Create + PS 直到灯条快闪），
然后：

```text
agent NoInputNoOutput
default-agent
pairable on
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
scan off
quit
```

重复配对同一手柄时，先执行 `remove XX:XX:XX:XX:XX:XX` 再 `scan on`。

注册手柄：`vdsctl attach <地址> --profile ds5 --ports 0`。之后按 PS 键，
`vdsd` 会自动恢复虚拟 USB 手柄。

### 音频与输入设置

把虚拟手柄的音频输出配置为 48 kHz 4 声道 S16_LE PCM。PipeWire/WirePlumber
下可将手柄声卡 profile 设为 `pro-audio`：

```sh
wpctl status
pw-cli e <device-id> EnumProfile
wpctl set-profile <device-id> <pro-audio-profile-index>
```

然后安装附带的 WirePlumber 规则（提供稳定显示名、4 声道输出、禁用
channelmix 归一化、降低麦克风优先级）：

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp 99-vds-dualsense-wireplumber.conf ~/.config/wireplumber/wireplumber.conf.d/
systemctl --user restart pipewire pipewire-pulse wireplumber
```

再安装 udev 规则，让虚拟手柄触摸板被归类为外部触摸板：

```sh
sudo cp 99-vds-dualsense-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

## 桌面音频触觉（Windows）

控制中心的「桌面音频触觉」可捕获任意 Windows 播放设备的系统混音，实时驱动
DualSense 的音圈致动器。

有两种发送模式，由 `%ProgramData%\vDS\haptics-mode.txt` 选择（改动后重启
`vdsd` 服务生效）：

| 取值 | 报告 | 说明 |
| --- | --- | --- |
| `native` | `0x36`（398 字节） | 直接发送 64 点原始音圈波形，细腻度等同 USB 直连 |
| `rumble` | `0x31`（78 字节） | 把波形压成左右两个 RMS 标量，由手柄固件合成振动 |

早期版本认为标准蓝牙适配器无法承载 398 字节的 `0x36` 报告，实测并非如此：
BE200 的蓝牙 HID 接口报告 `OutputReportByteLength = 547`，且 Windows HID 写入
本来就会把每个输出报告补零到该长度——因此发 `0x36` 与发 `0x31` 占用的空口
带宽完全相同。实测 `native` 模式可满速稳定运行（93.75 包/秒，零写入错误）。

未设置该文件时默认为 `rumble`，保持与旧行为一致。

使用步骤：

1. 确保 vDS 服务运行、手柄已经连接，然后打开 `VdsGui.exe`。
2. 在「桌面音频触觉」中选择扬声器/耳机等实际播放设备，点击「开始音频触觉」。
3. 从「平衡」预设开始，逐步提高「触觉强度」；若持续震动太明显，增加「低切」
   或提高「噪声门限」。
4. 高级面板可以调节左右马达、声道映射、立体声宽度、频带、压缩、起音/释放
   和输出上限；所有参数会立即应用并保存在
   `%LocalAppData%\vDS\audio-haptics.json`。
5. 「音频混音 / 原生音圈」面板提供音圈增益（建议 1.0–5.0）、每个马达的
   源声道映射，以及 3 段 / 6 段均衡（`OFF` / `BAND_3` / `BAND_6`，低架 +
   峰值 + 高架，增益 ±20 dB）。

控制中心不会把实体或虚拟 DualSense 的音频端点列为捕获来源，避免把手柄
扬声器输出再次转换成触觉。若音频触觉早于手柄连接启动，新建立的手柄桥接
也不会自动加入这段已存在的流；请在手柄连接后停止并重新开始音频触觉。

> [!NOTE]
>
> 触觉通道在 `PcmAudioExtractor` 中被 16:1 抽取到 3 kHz，因此奈奎斯特频率
> 约为 1.5 kHz。更高的顶部频段对纯触觉通道不起作用（这类频段通常同时服务
> 扬声器与耳机）；界面会把这些频段标注为「对振动无效」。

> [!IMPORTANT]
>
> 从 `rumble` 切到 `native` 后，原先为补偿 RMS 损失而调高的参数（触觉强度、
> 输入增益、左右马达百分比）通常会过冲，把波形顶进 int8 上限（±127）而产生
> 削波失真。建议先把这些回到 100% / 0 dB 附近，再按需要小幅上调。

该功能由 GUI 所在的登录会话完成 WASAPI loopback 捕获，所以音频触觉开启期间
请保持控制中心运行。它通过专用低延迟管道把固定 48 kHz 数据块交给后台服务，
再按每块的左右能量包络生成马达报告；缓冲滑条仍可用于寻找当前蓝牙环境下的
最低稳定延迟。

### 手柄扬声器 / 耳机输出

`0x36` 报告每包携带一个 200 字节的 Opus 音频块，标签按手柄耳机口状态自动
选择内置扬声器（`0x13`）或 3.5mm 耳机（`0x16`）。勾选面板里的「同时输出到
手柄扬声器 / 耳机」后，桌面音频的原始信号（不经触觉 DSP）会与音圈波形一起
送到手柄——插上耳机即可把手柄当无线音频接收器用。

- 仅在 `native` 触觉模式下有效（`0x31` 马达报告没有音频块）；
- 注入管道协议为 v2（48 kHz、4 声道：扬声器 L/R + 触觉 L/R），守护进程仍
  兼容旧的 v1 双声道纯触觉流；
- 游戏通过虚拟 USB 播放音频时，游戏音频优先占用扬声器通道。

## 扳机与灯效

控制中心的「扳机与灯效」面板（或 `vdsctl effects`）可以在游戏之外设置
自适应扳机、灯条颜色、玩家指示灯和麦克风静音灯。默认与游戏协作：游戏
主动使用某项功能时自动让位，勾选强制模式则始终以面板设置为准。详见
[docs/effects-CN.md](docs/effects-CN.md)。

```powershell
& "C:\Program Files\vDS\vdsctl.exe" effects --led 0,60,255 --right-trigger weapon:2,6,8
```

## DSX 兼容 API

`vdsd` 同时提供与 DSX 公开 UDP 协议兼容的接口（默认
`udp://127.0.0.1:6969`），为 DSX 编写的游戏 mod 无需修改即可驱动 vDS 的
扳机与灯效。通过 `%ProgramData%\vDS\dsx-api.txt` 写入 `off` 或端口号可
禁用/改端口。详见 [docs/dsx-api-CN.md](docs/dsx-api-CN.md)。

## 查看手柄信息

桥接建立后，`vdsctl info` 通过蓝牙 HID feature report 读取实体手柄的身份
信息（与 https://ds.evua.cc/ 等网站显示的数据相同）：

```powershell
vdsctl info
```

守护进程在每次桥接启动时读取一次并缓存，输出包括型号、外壳序列号、固件
更新版本（`A-xxxx`）、主板型号（`HMB-010` / `BDM-xxx` / `HDM-010`）、生产
时间、颜色代码/名称、手柄 MAC 地址，以及 DualSense Edge 的摇杆模块锁定状态。
同样信息会写入守护进程日志（`controller info` 行），并在控制中心的手柄列表
中显示。

## 验证和排错

### Windows

- 运行 `joy.cpl`，应看到虚拟有线 DualSense；支持触觉音频的游戏还会看到
  手柄的扬声器、耳机和麦克风端点。
- 查看状态：`& "C:\Program Files\vDS\vdsctl.exe" list`。
- 查看可配对目标：`& "C:\Program Files\vDS\vdsctl.exe" list-targets`。
- 日志位于 `%ProgramData%\vDS\vdsd.log`。
- 详细跟踪：先执行 `vdsctl trace on --scope all`，复现问题后执行
  `vdsctl trace off --scope all`，避免日志持续增长。

若游戏仍显示两台手柄，确认 HidHide 已安装、`vdsd` 服务正在运行，并完全退出
DS4Windows、Steam Input 或其他手柄映射器后重试。

### Linux

```sh
lsusb -d 054c:            # 检查虚拟 USB 手柄是否枚举
evtest                    # 检查输入设备
fftest /dev/input/eventX  # 检查力反馈支持
aplay -l                  # 检查虚拟 USB 音频端点
speaker-test -D hw:<card>,<device> -c 4 -r 48000 -F S16_LE -t sine
```

## 报告问题

报告问题时请包含：平台、vDS 版本、手柄型号、连接方式（例如蓝牙适配器型号）、
受影响的应用程序或游戏，以及复现步骤。运行期问题请先打开跟踪再复现：

```sh
vdsctl trace on --scope all
vdsctl trace off --scope all
```

## 相关文档

- [README.md](README.md)：英文总览
- [README-WINDOWS.md](README-WINDOWS.md)：Windows 构建与安装指南
- [README-LINUX.md](README-LINUX.md)：Linux 构建与安装指南
- [docs/effects-CN.md](docs/effects-CN.md)：扳机与灯效
- [docs/dsx-api-CN.md](docs/dsx-api-CN.md)：DSX 兼容 API
- [docs/controller-output-test-CN.md](docs/controller-output-test-CN.md)：
  手柄输出测试
