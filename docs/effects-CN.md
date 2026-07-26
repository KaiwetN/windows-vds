# 扳机与灯效(守护进程侧控制器效果)

vDS 可以在游戏之外为手柄设置自适应扳机、灯条颜色、玩家指示灯和麦克风静音灯。
典型用途:给不使用 DualSense 特性的游戏补上扳机阻力和灯效,或把灯条固定成
喜欢的颜色。

## 仲裁语义:为什么效果能和游戏共存

DualSense 的 47 字节输出状态(`vds_set_state_data`,见
`include/vds/ds5_protocol.h`)里,每一类功能都由一个独立的 `allow_*` 位守护:
手柄固件只在报告中对应位被置起时才应用那一段字段。vDS 转发游戏输出报告的
`apply_usb_output_report()`(`src/vds_protocol.cc`)同样只在游戏自己置位时
覆盖对应字段。

因此:

- **默认(跟随游戏可覆盖)**:vDS 写入效果并置起 allow 位。不碰该功能的游戏
  不会覆盖它;真正使用该功能的游戏一旦写入,自动接管。
- **强制模式**(`--force on`):每次游戏输出报告应用之后,vDS 立即把自己的
  效果重写回去(`src/platform/win32/vdsd.cc` 的 `handle_virtual_frame`),
  游戏的设置被压制。
- **清除(`none`)**:vDS 停止主张该字段。注意这不会"撤销"手柄上已生效的
  值——手柄保持最后收到的状态,直到游戏(或新的效果)再次写入。

音频流活跃时,效果搭 `0x36` 触觉报告的车(每包携带完整状态);空闲时由
`0x31` 状态报告推送。两条路都不增加额外带宽。

## 命令行

```sh
vdsctl effects                                  # 查询当前效果
vdsctl effects --led 255,0,0                    # 灯条纯红
vdsctl effects --led none                       # 灯条交还游戏控制
vdsctl effects --player 4 --mute-led breath     # 中间玩家灯 + 麦克风灯呼吸
vdsctl effects --right-trigger weapon:2,6,8     # R2 武器段落阻力
vdsctl effects --left-trigger off               # L2 显式关闭效果
vdsctl effects --force on                       # 压制游戏自己的设置
vdsctl effects --clear                          # 全部清除
```

`--player` 取 0–63:低 5 位是 5 颗灯的位掩码(从左到右 bit0–bit4),bit5 是
淡入。常用值:`4`=中间一颗,`10`/`21`/`27`=2/3/4 号位样式,`31`=全亮。

`--mute-led` 取 `off` / `on` / `breath`(0/1/2)。

### 扳机 SPEC

| SPEC | 含义 | 参数 |
| --- | --- | --- |
| `off` | 关闭效果(模式 0x05) | — |
| `feedback:pos,strength` | 从 pos 起持续阻力 | pos 0–9, strength 1–8 |
| `weapon:start,end,strength` | 段落阻力后突断(扣扳机感) | start 2–7, end start+1–8, strength 1–8 |
| `vibration:pos,amplitude,freq` | 过 pos 后高频震动 | pos 0–9, amp 1–8, freq 1–255 Hz |
| `bow:start,end,strength,snap` | 弓弦张力 + 回弹 | start 0–8, end ≤8, 均 1–8 |
| `galloping:start,end,foot1,foot2,freq` | 马蹄节奏 | foot1 0–6, foot2 ≤7 |
| `machine:start,end,strA,strB,freq,period` | 机枪抖动 | str 0–7, period ×0.1s |
| `raw:<22位hex>` | 直接给出 11 字节效果数据 | — |

字节格式沿用社区对 DualSense 固件扳机模式的整理(Nielk1 的
TriggerEffectGenerator,MIT),构造器实现在 `src/vds_protocol.cc`
(`trigger_effect_*`)。

## 控制协议(daemon)

命令走 `\\.\pipe\vdsd` 的 JSONL 控制管道,与 `attach`/`audio-buffer` 同一
条通道。所有值都是字符串;缺省字段表示"保持不变",`"none"` 表示清除:

```json
{"command":"effects","led":"0,60,255","player":"4","mute_led":"2",
 "right_trigger":"weapon:2,6,8","force":"off"}
```

应答:

```json
{"OK":true,"error":"","effects":{"left_trigger":"","right_trigger":"2544000700000000000000",
 "led":"0,60,255","player":"4","mute_led":"2","force":"off"}}
```

应答中的扳机字段是解析后的 11 字节 hex,可原样通过 `raw:` 回放。

无副作用查询:发送只含 `command` 的请求。

## 持久化与优先级

- daemon 把生效状态存到 `%ProgramData%\vDS\effects.json`,服务重启后自动
  恢复并在手柄连接时应用。
- GUI「扳机与灯效」面板把自己的选择存到 `%LocalAppData%\vDS\effects-ui.json`,
  并在每次启动时重新应用一次(仅当该文件已存在,避免新装 GUI 清掉你用
  vdsctl 配好的效果)。
- 后写者胜:GUI 和 vdsctl 修改的是同一份 daemon 状态。

## 实现位置

| 组件 | 位置 |
| --- | --- |
| 效果结构与扳机构造器 | `src/vds_protocol.hh` / `src/vds_protocol.cc` |
| allow 位写入 | `DsOutputState::apply_authored_effects` |
| daemon 存储/命令/持久化 | `src/platform/win32/vdsd.cc`(`EffectsStore`、`handle_effects_control_command`) |
| 应用与强制重写 | `apply_authored_effects_if_changed`(flush 线程)、`handle_virtual_frame` |
| CLI | `src/vdsctl_common.cc`(`run_vdsctl_effects`) |
| GUI | `gui/EffectsSettings.cs`、`gui/MainWindow.xaml(.cs)` 「扳机与灯效」卡片 |

## 已知限制

- Linux daemon 尚未接入该命令(共享协议层已具备,缺少 daemon 侧处理)。
- 清除(`none`)不会主动把手柄恢复到"无效果",只是停止主张;需要立即
  复原时用显式值(如 `--left-trigger off`、`--led 0,0,255`)。
- 玩家指示灯亮度、灯条淡入动画(`light_fade_animation`)暂未暴露。
