# DSX 兼容对外 API

`vdsd` 内置一个与 DSX 公开 UDP 协议兼容的接口,默认监听
`udp://127.0.0.1:6969`。为 DSX 编写的游戏 mod 和工具(通过 UDP 发送
instructions 包的那一类)无需修改即可驱动 vDS 的扳机与灯效。

协议标识符(`InstructionType`、`TriggerModes`、`CustomTriggerValueMode` 等)
与社区公开的 DSX UDP 示例一致。

## 报文格式

单个 UDP 数据报,UTF-8 JSON:

```json
{"instructions":[{"type":1,"parameters":[0,1,15,0,9,2,5,4]},
                 {"type":2,"parameters":[0,170,0,255]}]}
```

一个包可携带多条指令,按顺序应用,整包只触发一次下发。

## 指令表

| type | 名称 | parameters | vDS 行为 |
| --- | --- | --- | --- |
| 0 | GetDSXStatus | `[...]` | 忽略(暂不回包;绝大多数客户端即发即忘) |
| 1 | TriggerUpdate | `[手柄序号, 扳机(1=L2,2=R2), TriggerMode, 参数...]` | 设置对应扳机效果 |
| 2 | RGBUpdate | `[手柄序号, R, G, B]` | 设置灯条颜色 |
| 3 | PlayerLED | `[手柄序号, 玩家编号 0–5]` | 设置玩家指示灯样式 |
| 4 | TriggerThreshold | `[手柄序号, 扳机, 阈值]` | 忽略(输入侧概念,游戏直接读真实 DualSense) |
| 5 | MicLED | `[手柄序号, 模式]` (0=On, 1=Pulsing, 2=Off) | 设置麦克风静音灯 |
| 6 | PlayerLEDNewRevision | 同 3 | 同 3 |
| 7 | ResetToUserSettings | `[手柄序号]` | 清除全部 vDS 侧效果 |

手柄序号目前忽略(单手柄语义,作用于当前桥接的控制器)。

## TriggerMode 映射

命名预设按社区对照表近似映射到 DualSense 固件的原生效果模式
(实现:`src/platform/win32/vdsd.cc` 的 `dsx_api::trigger_from_instruction`):

| mode | 预设名称 | vDS 映射 |
| --- | --- | --- |
| 0 | Normal | off |
| 1 | GameCube | weapon 3,6,6 |
| 2–7 | VerySoft…Rigid | feedback,强度 2/3/6/7/8/8 |
| 8 | VibrateTrigger | vibration,频率取参数(默认 20 Hz) |
| 9 | Choppy | machine 0,9,7,7,10,3 |
| 10 | Medium | feedback 强度 5 |
| 11 | VibrateTriggerPulse | vibration 10 Hz |
| 12 | CustomTriggerValue | 见下 |
| 13 | Resistance | feedback:start,force |
| 14 | Bow | bow:start,end,strength,snap |
| 15 | Galloping | galloping:start,end,foot1,foot2,freq |
| 16 | SemiAutomaticGun | weapon:start,end,strength |
| 17 | AutomaticGun | machine:start,9,str,str,freq |
| 18 | Machine | machine:全参数 |

超范围参数会被钳制到合法区间而不是报错;无法构造时回落为 off。

`CustomTriggerValue`(mode 12)按社区整理的旧版固件模式字节表直发:
参数第 1 个是 `CustomTriggerValueMode`(OFF/Rigid/RigidA/RigidB/RigidAB/
Pulse/PulseA/PulseB/PulseAB/VibrateResistance*/VibratePulse*),其余参数
作为效果数据字节 1–7 原样传入。

## 与 vdsctl / GUI 的关系

三者写的是同一份 daemon 效果状态,后写者胜:

- API 更新**不落盘**(游戏运行时高频推送,持久化无意义且伤盘);
- `vdsctl effects` 与 GUI 面板的修改会持久化到
  `%ProgramData%\vDS\effects.json`;
- `ResetToUserSettings` 清除的是内存态;服务重启后仍会从 effects.json
  恢复用户配置。

仲裁语义(与游戏 USB 输出的协作/强制)见
[effects-CN.md](effects-CN.md)。

## 配置

`%ProgramData%\vDS\dsx-api.txt`(修改后重启 `vdsd` 服务):

| 内容 | 含义 |
| --- | --- |
| 文件不存在 | 启用,端口 6969(DSX 默认) |
| `off` | 禁用 |
| 数字 | 启用,自定义端口 |

仅监听回环地址。端口被占用(例如 DSX 本体在跑)时,vDS 记一条警告并
禁用该接口,不影响其余功能。

## 快速验证

```powershell
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect("127.0.0.1", 6969)
$b = [Text.Encoding]::UTF8.GetBytes('{"instructions":[{"type":2,"parameters":[0,255,0,0]}]}')
[void]$udp.Send($b, $b.Length)   # 灯条变红
```

## 已知限制

- `GetDSXStatus` 不回包;依赖状态应答做握手的客户端会认为 DSX 未运行。
- 手柄序号被忽略,多手柄场景所有指令作用于同一控制器。
- 命名预设(GameCube/Choppy 等)的手感是近似,不保证与原始软件逐字节一致;
  需要精确控制时用 mode 12(CustomTriggerValue)或 13–18 带参模式。
