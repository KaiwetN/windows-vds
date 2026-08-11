# 用 BE200 把蓝牙 DualSense 模拟成有线手柄

这个 Windows 程序让 Intel BE200 连接实体 DualSense / DualSense Edge，随后向
Windows 暴露一台完整的虚拟 USB DualSense。游戏看到的是 Sony USB 设备，而不是
普通 XInput 映射。

它支持按键、触摸板、陀螺仪、灯效、普通震动、自适应扳机、四声道触觉音频、
手柄扬声器/耳机和麦克风。固件更新不能通过虚拟设备进行。

本目录基于 MIT 许可的
[hurryman2212/vds](https://github.com/hurryman2212/vds) `0.4.0-rc1`。
协议转换部分继承了 DS5Dongle 的研究成果；来源记录见 `UPSTREAM.md`。

## 为什么 BE200 可以用

BE200 的 Windows 蓝牙驱动能够把 DualSense 暴露为蓝牙 HID 设备。vDS 直接读写
这个 HID 设备，再通过 `usbip-win2` 创建虚拟 USB 设备；因此不需要在 BE200 上运行
自定义 Bluetooth Classic 固件，也不需要 ESP32-S3 或 Pico 2 W。

要求 64 位 Windows 10 1903 或更新版本。建议先通过 Windows Update 或 Intel
Driver & Support Assistant 更新 BE200 蓝牙驱动。

## 一次性安装

先在普通 PowerShell 中编译：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\build-windows.ps1
```

脚本会自动使用 Visual Studio 2022、其内置 CMake 和 vcpkg，并生成 x64 Release
版本，同时生成中文 GUI：

```text
out\gui\VdsGui.exe
```

双击 GUI 会请求管理员权限。它可以安装/更新 vDS、显示蓝牙与虚拟 USB 状态、
注册或取消注册手柄、打开 Windows 手柄测试以及查看服务日志。也可以继续使用下方
命令行安装方式。打开“以管理员身份运行”的 PowerShell：

```powershell
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\setup-windows.ps1 -Mode Check
.\setup-windows.ps1 -Mode Install
```

安装模式会从官方 GitHub Release 下载并验证签名，然后安装：

- `usbip-win2`：把本机 USB/IP 端点导入为 Windows USB 设备；
- `HidHide`：隐藏实体蓝牙 HID，避免游戏收到两份输入；
- `vdsd`：自动启动的桥接服务；
- `vdsctl`：手柄注册和诊断工具。
- `VdsGui`：中文图形控制中心，并添加到开始菜单的 `vDS` 文件夹。

> **注意：** 安装 USB/IP 驱动会重启 USB 3 Hub，键鼠等 USB 设备可能短暂断开。
> 如果脚本提示需要重启，请先重启 Windows。

## 配对并连接

1. 关闭可能独占手柄的 DS4Windows；首次验证时也建议暂时退出 Steam。
2. 在手柄关机时同时按住 **Create（触摸板左上方）+ PS**，直到灯条快速闪烁。
3. 打开 Windows“设置 → 蓝牙和设备 → 添加设备 → 蓝牙”，选择
   **Wireless Controller**。
4. 配对完成后按一次 PS 键，让手柄处于在线状态。
5. 在普通 PowerShell 中运行：

```powershell
cd "C:\Users\Administrator\Documents\New project\windows-vds"
.\connect-controller.ps1
```

只有一个已配对手柄时脚本会自动选择它，并自动分配空闲虚拟 USB 端口。多个手柄
可指定蓝牙地址；仅在需要固定端口时再传入 `-Port`：

```powershell
.\connect-controller.ps1 -Address "aa:bb:cc:dd:ee:ff" -Port 0
```

DualSense Edge 会被自动识别；也可通过 `-Profile ds5` 强制把它作为普通 DualSense
暴露，以兼容不认识 Edge 的游戏。

手柄注册信息保存在 `%ProgramData%\vDS\vdsd.db`。以后只需按 PS 键，后台服务会
自动恢复虚拟 USB 手柄，不必重复运行连接脚本。

## 桌面音频触觉

控制中心的“桌面音频触觉”可捕获任意 Windows 播放设备的系统混音，实时驱动
DualSense 的音圈致动器。

有两种发送模式，由 `%ProgramData%\vDS\haptics-mode.txt` 选择（改动后重启
`vdsd` 服务生效）：

| 取值 | 报告 | 说明 |
| --- | --- | --- |
| `native` | `0x36`（398 字节） | 直接发送 64 点原始音圈波形，细腻度等同 USB 直连 |
| `rumble` | `0x31`（78 字节） | 把波形压成左右两个 RMS 标量，由手柄固件合成振动 |

早期版本认为标准蓝牙适配器无法承载 398 字节的 `0x36` 报告，实测并非如此：
BE200 的蓝牙 HID 接口报告 `OutputReportByteLength = 547`，且 Windows HID 写入
本来就会把每个输出报告补零到该长度——因此发 `0x36` 与发 `0x31` 占用的空口带宽
完全相同。实测 `native` 模式可满速稳定运行（93.75 包/秒，零写入错误）。

未设置该文件时默认为 `rumble`，保持与旧行为一致。

### 手柄扬声器 / 耳机输出

`0x36` 报告每包携带一个 200 字节的 Opus 音频块，标签按手柄耳机口状态自动
选择内置扬声器（`0x13`）或 3.5mm 耳机（`0x16`）。勾选面板里的
「同时输出到手柄扬声器 / 耳机」后，桌面音频的原始信号（不经触觉 DSP）会
与音圈波形一起送到手柄——插上耳机即可把手柄当无线音频接收器用。

- 仅在 `native` 触觉模式下有效（`0x31` 马达报告没有音频块）；
- 注入管道协议为 v2（48 kHz、4 声道：扬声器 L/R + 触觉 L/R），守护进程
  仍兼容旧的 v1 双声道纯触觉流；
- 游戏通过虚拟 USB 播放音频时，游戏音频优先占用扬声器通道。

1. 确保 vDS 服务运行、手柄已经连接，然后打开 `VdsGui.exe`。
2. 在“桌面音频触觉”中选择扬声器/耳机等实际播放设备，点击“开始音频触觉”。
3. 从“平衡”预设开始，逐步提高“触觉强度”；若持续震动太明显，增加“低切”或提高
   “噪声门限”。
4. 高级面板可以调节左右马达、声道映射、立体声宽度、频带、压缩、起音/释放和输出
   上限；所有参数会立即应用并保存在 `%LocalAppData%\vDS\audio-haptics.json`。
5. “音频混音 / 原生音圈”面板提供音圈增益
   （建议 1.0–5.0）、每个马达的源声道映射
   （源声道映射）、以及 3 段 / 6 段均衡
   （`OFF` / `BAND_3` / `BAND_6`，低架 + 峰值 +
   高架，增益 ±20 dB）。

控制中心不会把实体或虚拟 DualSense 的音频端点列为捕获来源，避免把手柄扬声器
输出再次转换成触觉。若音频触觉早于手柄连接启动，新建立的手柄桥接也不会自动加入
这段已存在的流；请在手柄连接后停止并重新开始音频触觉。

> [!NOTE]
>
> 触觉通道在 `PcmAudioExtractor` 中被 16:1 抽取到 3 kHz，因此奈奎斯特频率约为
> 1.5 kHz。更高的顶部频段通常同时服务扬声器与耳机，
> 对纯触觉通道不起作用；界面会把这些频段标注为“对振动无效”。

> [!IMPORTANT]
>
> 从 `rumble` 切到 `native` 后，原先为补偿 RMS 损失而调高的参数（触觉强度、
> 输入增益、左右马达百分比）通常会过冲，把波形顶进 int8 上限（±127）而产生
> 削波失真。建议先把这些回到 100% / 0 dB 附近，再按需要小幅上调。

该功能由 GUI 所在的登录会话完成 WASAPI loopback 捕获，所以音频触觉开启期间请保持
控制中心运行。它通过专用低延迟管道把固定 48 kHz 数据块交给后台服务，再按每块的
左右能量包络生成马达报告；缓冲滑条仍可用于寻找当前蓝牙环境下的最低稳定延迟。

## 扳机与灯效

控制中心的「扳机与灯效」面板(或 `vdsctl effects`)可以在游戏之外设置
自适应扳机、灯条颜色、玩家指示灯和麦克风静音灯。默认与游戏协作:游戏
主动使用某项功能时自动让位,勾选强制模式则始终以面板设置为准。详见
[docs/effects-CN.md](docs/effects-CN.md)。

```powershell
& "C:\Program Files\vDS\vdsctl.exe" effects --led 0,60,255 --right-trigger weapon:2,6,8
```

## DSX 兼容 API

`vdsd` 同时提供与 DSX 公开 UDP 协议兼容的接口(默认
`udp://127.0.0.1:6969`),为 DSX 编写的游戏 mod 无需修改即可驱动 vDS 的
扳机与灯效。通过 `%ProgramData%\vDS\dsx-api.txt` 写入 `off` 或端口号可
禁用/改端口。详见 [docs/dsx-api-CN.md](docs/dsx-api-CN.md)。

## 验证和排错

- 运行 `joy.cpl`，应看到虚拟有线 DualSense；支持触觉音频的游戏还会看到手柄的
  扬声器、耳机和麦克风端点。
- 查看状态：`& "C:\Program Files\vDS\vdsctl.exe" list`。
- 查看可配对目标：`& "C:\Program Files\vDS\vdsctl.exe" list-targets`。
- 日志位于 `%ProgramData%\vDS\vdsd.log`。
- 详细跟踪：先执行 `vdsctl trace on --scope all`，复现问题后执行
  `vdsctl trace off --scope all`，避免日志持续增长。

若游戏仍显示两台手柄，确认 HidHide 已安装、`vdsd` 服务正在运行，并完全退出
DS4Windows、Steam Input 或其他手柄映射器后重试。
