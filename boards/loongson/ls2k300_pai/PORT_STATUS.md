# Loongson 2K0300 (LS2K300-PAI) 上的 Zephyr 移植：进度与方向

本文记录这个 Zephyr 分支（基于 v4.4.2）在龙芯 2K0300 / 先锋派（LS2K300-PAI）开发板上的
实际状态：已验证可用的部分、每一处关键修复的原因、已知风险与下一步方向。
硬件实测的原始寄存器事实集中在 `references/hardware-registers.md`（配套 skill）里。

## 1. 运行方式（已固定）

| 环节 | 做法 |
|---|---|
| 引导 | u-boot → TFTP 取 `zephyr.uImage` → `bootm ${loadaddr}`（`loadaddr=0x03000000`） |
| TFTP 根目录 | `/mnt/h/Zephyr`（`zephyr.uImage` 为当前活动镜像，历史镜像按 `zephyr_*.uImage` 留档） |
| 构建 | `west build -p always -b ls2k300_pai -d build/<name> <sample> -DEXTRA_CONF_FILE="<confs>"`，工具链变量 `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`、`ZEPHYR_SDK_INSTALL_DIR=/home/kyoko/zephyr-sdk-1.0.1` |
| 必备增量配置 | `build/pai_bringup_shell.conf`（UART 修复 + shell/devmem/i2c，**本板必须带**）；LVGL 另加 `build/pai_lvgl.conf` |
| 常用镜像 | `zephyr.uImage`（当前题目）、`zephyr_display.uImage`（SSD1306 示例）、`zephyr_blinky.uImage`、`zephyr_lvgl.uImage` |

板上可用的调试手段：`i2c scan/read/write/speed/recover`、`devmem <addr> [width] [value]`、
`led`、LVGL 自带 `lvgl` 命令、`kernel`/`log` 等系统命令。

## 2. 已完成并硬件验证

- **控制台（uart0）**：收发可用、shell 可交互（多轮长会话验证）。
- **定时器**：`rdtime/TCFG` 自标定，`SYS_CLOCK_HW_CYCLES_PER_SEC=120000000`（原 100 MHz 使所有延时短 20%）。
- **GPIO / LED / pinctrl**：`gpio-leds` 与 shell 命令可用；引脚复用驱动（RMW 单字段）正常。
- **I2C 主机（4 个控制器）**：`i2c0` 已启用，`i2c1` 挂 SSD1306；4 个节点与主/备引脚组均在设备树里。
- **SSD1306（128×64，i2c1）**：初始化、开屏、清屏、整屏命令均生效。
- **LVGL**：单色 128×64 上跑通（显示 "Hello World" + 计数器），控制台可同时使用。
- **I2C 速率**：100 kHz 与 400 kHz 均可切换并实测正常（`i2c speed i2c1 1|2`）。

## 3. 关键修复清单（含原因）

### 3.1 定时器
- u-boot 让计数器在跑，**En 置位时写 TCFG 会被忽略** → 驱动改为先停表再编程；
- `InitVal` 单位不是想当然的 1 个周期 → 驱动在 init 时**实测**"1 unit = 多少 rdtime 周期"；
- 根因修复：`soc/loongson/ls2k0300/Kconfig.defconfig` 里 100 MHz → **120 MHz**（dtsi 的 `osc_clk` 与实测一致）。

### 3.2 控制台 UART（两个真实的 ns16550 行为问题）
- **请求只拉一次**：第一个字符被服务后，后续字符再也拿不到中断 → 打开
  `CONFIG_UART_NS16550_WA_ISR_REENABLE_INTERRUPT`（每次 ISR 末尾翻转 IER 重新 assert）；
- **TX FIFO 已空时 "TX empty" 中断不产生** → 打开 `CONFIG_UART_NS16550_WA_TX_FIFO_EMPTY_INTERRUPT`；
- 厂商设备树给这颗 UART 标了 `no-loopback-test`：**回环态（MCR.LOOP=1 且 OUT2=0）会让接收端离开 RX 引脚、
  并把中断输出关掉**，症状是"能打印、完全收不到、还把 shell 自己的输出当输入"；
- 启动期确定性写入：`FCR=0x07`（单字节触发）、`MCR=0x0b`（关回环、OUT2 有效）、GPIO40/41 复用为主功能 3；
- 控制台看门狗（`CONFIG_LS2K0300_CONSOLE_GUARD`，默认开、静默自愈）：引脚复用被改回、回环被清、
  接收/发送请求丢失时翻 IER 重触发、发送卡死时清 FIFO。它只在修复时写寄存器，记录走 `LOG_DBG`。

### 3.3 I2C（这一轮工作量最大，且最初方向是错的）
- **IP 认错**：6.12 设备树里的 `loongson,lsfs-i2c` 配不到任何驱动，早期照 PCF8584 风格的
  `i2c-ls2x.c` 实现 → 完全不通。正确来源是 **u-boot `drivers/i2c/ls2k300_i2c.c`** 与
  **新内核 `drivers/i2c/busses/i2c-ls2x-v2.c`**（二者寄存器定义一致）：32 位寄存器、
  `CR1/CR2/DR(0x10)/SR1(0x14)/SR2(0x18)/CCR(0x1c)/TRISE(0x20)`，兼容串 `loongson,ls2k0300-i2c`；
- **写流程**：中间字节等 **TXE**、末字节等 **BTF**（BTF 逐字节会超时，是实测结论）；CR2 按参考实现
  置 `ITERREN|ITEVTEN|ITBUFEN`（事件标志的生成与之相关，且本端口未接 I2C 中断线，不会产生中断风暴）；
- **消息成帧**：SSD1306 驱动的"控制字节 + 数据"是两条同方向消息，中间**不能**重发 START。
  参考实现在非末条消息后才发 RepStart，而这里实测：**同方向消息必须作为一笔事务连续发**，
  否则地址被应答、传输报成功、器件却毫无反应。现在只在**方向变化**（读寄存器）时才重发 START+地址；
- **速度常量语义**：本树 `I2C_SPEED_*` 是**未移位**编码（STANDARD=1、FAST=2），必须用
  `I2C_SPEED_GET()` 解码；直接比较移位后的字段会把 "1" 悄悄当成快模式、并把 "2" 判为不支持；
- **CCR 夹取**：只夹 12 位分频字段，不能把 `FS` 位（bit15）一起夹掉，否则 400 kHz 会退化成约 24 kHz；
- **NACK 不再复位控制器**：扫描探测空地址属正常总线行为，旧行为会 `SWRST` 打断正在进行的显示传输；
- 失败日志用 **`printk`**（见 4.1 的 LOG 缺陷），格式含 SR1/SR2/CR1/CR2，便于下次定位。

### 3.4 SSD1306 + LVGL（单色面板的四个必要条件）
1. `CONFIG_LV_COLOR_DEPTH_1=y` —— LVGL 默认 16 位色，单色面板必须显式选 1bpp；
2. `CONFIG_LV_Z_VDB_SIZE=25` —— 默认 10% 在单色下约 6 行，LVGL 行取整失败
   （`get_max_row ... too small draw_buf`）并走降级渲染；
3. `CONFIG_TIMESLICING=y` + `TIMESLICE_SIZE=1` + `TIMESLICE_PRIORITY=14` —— UI 与 shell 同优先级，
   Zephyr 默认不轮转，一次阻塞 I2C 刷新（整帧约 92 ms@100 kHz）就能把控制台锁死；
4. `CONFIG_MAIN_STACK_SIZE=8192` —— 单色竖排（VTILED）多一层转换缓冲，4 KB 栈偏紧。
   另外 `LV_Z_MONOCHROME_CONVERSION_BUFFER=y` 是 SSD1306 竖排路径所需（默认已开）。

## 4. 已知问题与风险

### 4.1 【P0，未修】端口级多参数 `LOG_*` 会错位、可致 TLB 崩溃
- 现象：多参数 `LOG_ERR/LOG_DBG` 打印出的值**整体错位**（寄存器位置出现别的值），
  其中一次在格式化器里触发 **TLB 异常**（`TLBRBADV=0x0000000200000040`，返回地址在 RAM），系统挂死；
- 单参数日志正常，`printk` 多参数正常（控制台看门狗一直用它，未见异常）；
- 现状规避：I2C 驱动**完全不使用 `LOG_*`**，失败信息一律 `printk`；
- 待办：定位该 64 位目标上 log 参数打包/字符串段的问题（怀疑与参数按格式推断宽度有关），
  这是目前唯一会"无预警炸系统"的缺陷。

### 4.2 其它
- `i2c3` 主引脚组是 G54/G55，而**板上 LED 就是 GPIO54**：默认保持关闭；要用 i2c3 请选备用组
  `i2c3_g84_default/g85_default`（G84/G85，功能 2）。`i2c1`/`i2c2` 也默认关闭，按需 `status="okay"`。
- 400 kHz 已验证可用；若现场换更长线缆或更弱上拉，出现花屏时先退回 100 kHz。
- LVGL 整帧刷新在 100 kHz 下约 92 ms，建议运行时 `i2c speed i2c1 2`（400 kHz，约 23 ms）。

## 5. 下一步方向（建议顺序）

1. **在板上验证第 6 节的 EIOINTC 链路与第 7 节的 RTC**（一个 shell 会话即可：能输入 + 四条 `devmem`
   读回 + `rtc set/get rtc0`），确认 `IEN` 的读写语义与 `MAP` 编码后再往下加外设中断；
2. **修 4.1 的 LOG 缺陷**（P0：稳定性）；
2. 生产镜像收尾：去掉 `CONFIG_I2C_SHELL/DEVMEM_SHELL/SHELL`，只留必要的诊断开关（看门狗默认开）；
3. 外设扩展（按需）：SPI0（G56–G59 已被 u-boot 配好）、PWM、GMAC、USB 等，均沿用"u-boot + 新内核
   双参考 + devmem 现场核对"的方法；
4. 上游化准备：`i2c_loongson_ls2k0300.c`、`loongson,ls2k0300-i2c` 绑定、板级 dts 都可以整理成
   RFC 补丁；`PORT_STATUS.md` 与 `.vscode/skills/ls2k0300-port/` 可作为附件说明实测依据
   （后者与本仓库既有的 `zephyr-loongarch-port`、`vendor-doc-to-text` 两个 skill 并列）。

## 6. 中断控制器切到 EIOINTC（本轮改动，待硬件验证）

2K0300 把每个外设中断同时接到传统 LIOINTC 和扩展 IO 中断控制器（手册叫 EXTIOI，Linux 侧
叫 EIOINTC）。传统线是**多条外设共用**的（uart2~uart5 共用 liointc0 子线 2，uart6~uart9 共用
子线 3），静态 ISR 表下一条线只能挂一个 handler；GPIO 的中断更是只从扩展控制器引出。因此
这个端口把中断系统切到 EIOINTC，并在其后**移除了过时的 LIOINTC**（驱动 `intc_liointc.c`、
`Kconfig.liointc`、绑定 `loongson,liointc.yaml` 与两个设备树节点都已删除；硬件寄存器事实留在
`.vscode/skills/ls2k0300-port/references/hardware-registers.md`，需要时可按 git 历史恢复）。

| 项 | 值 |
|---|---|
| 设备树节点 | `dts/loongarch/loongson/ls2k0300.dtsi` 的 `eiointc`（`loongson,ls2k0300-eiointc`） |
| 驱动 | `drivers/interrupt_controller/intc_eiointc.c`（`CONFIG_INTC_EIOINTC`，绑定同目录 yaml） |
| 平坦中断号 | EXTIOI 向量 0..127 = IRQ 16..143（`NUM_IRQS=144`，唯一控制器） |
| 级联 | CPU 线 3（INT1，`ESTAT.IS[3]`），与厂商设备树一致；线 2/4（LIOINTC）已不再使用 |
| 主使能 | 芯片通用配置寄存器 0（`0x16000100`）bit 19 `extioi_en`，复位 0，由驱动打开 |
| uart0（控制台） | EXTIOI 向量 0 → `interrupts = <16 1>`、`interrupt-parent = <&eiointc>` |

向量分配见手册表 3-48：uart0..9 = 0..9、i2c0..3 = 10..13、spi2/3 = 14/15、
can0..3 的 core/buffer = 16..23、pwm0..3 = 28..31、apb-dma0..7 = 45..52、
rtc-int0..2 = 62..64、gpio 每 4 根引脚一个向量 = 79..105。

寄存器（系统寄存器窗口内偏移，手册 3.5.2，与 3A5000/2K0500 的同名 IP 偏移一致）：
使能 `IEN0..3` @0x1600（读改写）、极性 `POL0..3` @0x1640、设备状态 `ISR0..3` @0x1700
（不受使能位影响）、路由到核的状态 `CORE_ISR0..3` @0x1800（写 1 清除）、
向量组路由 `MAP` @0x14c0（每组 32 个向量一个 4 bit 独热字段：0001=INT0…1000=INT3）。

驱动 init：打开 `extioi_en` → 屏蔽所有向量并清 `CORE_ISR` → 把四个向量组全部路由到级联线 →
经 `z_loongarch_sub_intc_register()` 注册 → `IRQ_CONNECT` 父线 3 → 使能父线。派发：读
`CORE_ISR`，回写清除，再按平坦号调用 `_sw_isr_table[irq]`。

### 板端验证

```text
devmem 0x8000000016000100 4    # bit19 必须为 1（extioi_en 已打开）
devmem 0x80000000160014c0 4    # 期望 0x02020202（四组向量都 → INT1 = CPU 线 3）
devmem 0x8000000016001600 4    # 期望 bit0=1（uart0 的向量已使能）
devmem 0x8000000016001800 4    # 空闲应为 0；有未取走的字符时 bit0=1
```

功能判据就是控制台：能输入即证明"UART 请求 → EXTIOI 向量 0 → CPU 线 3 → 派发 → 驱动取走字符"
整条链成立（`CONFIG_LS2K0300_RX_CHECK` 的自检已同步改为按 EIOINTC 采样，开启后会打印
`isr=… core=… ien=… map=… cfg0=…` 三态）。

### 回退

LIOINTC 已从树里删掉，回退要走 git：

```bash
git checkout -- drivers/interrupt_controller/intc_liointc.c \
                drivers/interrupt_controller/Kconfig.liointc \
                dts/bindings/interrupt-controller/loongson,liointc.yaml
# 再按被删的两个节点（原第 226-261 行附近）把 liointc0/liointc1 加回 dtsi、
# 把 NUM_IRQS 调回 208，并把外设的 interrupt-parent 指回 liointc0/liointc1
```

也可以只把 `soc/loongson/ls2k0300/Kconfig.defconfig` 的 `NUM_IRQS` 暂时放宽到 208，
LIOINTC0/1 分别占 144..175 / 176..207（指针与寄存器事实见 skill 的寄存器文档）。

### 仍需在板上确认的点

- `IEN`（0x1600）按"可读写掩码"使用（读改写）；手册正文说"写 1 使能"，表格 3-48 的访问
  属性抽取列作 `R`，两者不一致，需 devmem 实测确认（若为只写 1 置位型，disable 会失效）。
- 脉冲型中断源的清除路径：驱动只回写清除 `CORE_ISR`（路由到核的状态），未动设备状态
  `ISR/ICLR`（0x1700），避免吞掉脉冲；若日后出现脉冲源重复触发，再评估是否需要写 0x1700。
- `MAP` 的字段编码（4 bit 独热）与 `INT0..INT3` → CPU 线 2..5 的对应关系取自手册表 3-51，
  并以 LIOINTC 时代的实测为旁证（当时的 INT0 = `ESTAT.IS[2]`，该驱动现已移除）；
  驱动按"父线 = 2 + INT 序号"推导 MAP 值，改父线只需改 DT。

## 7. RTC（TOY 墙上时钟）驱动（本轮新增，待硬件验证）

2K0300 的 RTC 块（基址 `0x16128000`，16 KB 窗口）里有两个计数器：32.768 kHz 晶振驱动的自由计数器，
以及按年/月/日/时/分/秒（外加 0.1 s 位）计数的 **TOY** 计数器——后者才是墙上时钟。移植按主线
`drivers/rtc/rtc-loongson.c` 的 `loongson,ls2k0300-rtc` 分支：**该型号的 TOY 匹配闹钟不可用**
（主线把它归在 `LOONGSON_RTC_ALARM_WORKAROUND` 下并清掉 `RTC_FEATURE_ALARM`），所以只做读写时间，
不接中断。

| 项 | 值 |
|---|---|
| 驱动 | `drivers/rtc/rtc_loongson_ls2k0300.c`（`CONFIG_RTC_LOONGSON_LS2K0300`，随 DT 节点默认启用） |
| 绑定 | `dts/bindings/rtc/loongson,ls2k0300-rtc.yaml` |
| 设备树 | `dts/loongarch/loongson/ls2k0300.dtsi` 的 `rtc0`（`0x16128000`；无引脚、无中断，直接 `okay`） |
| 子系统 | `CONFIG_RTC=y` + `CONFIG_RTC_SHELL=y`（写在 `build/pai_bringup_shell.conf`，与 I2C 同一做法） |

寄存器（手册第 25 章，偏移相对基址）：`toytrim 0x20`、`toywrite0/1 0x24/0x28`、`toyread0/1 0x2c/0x30`、
`rtcctrl 0x40`（REN bit13、TEN bit11、EO bit8；高位是只读"写状态"位）、`rtctrim 0x60`。
TOY 低位寄存器布局：MON[31:26] DAY[25:21] HOUR[20:16] MIN[15:10] SEC[9:4] 0.1s[3:0]，
年份在 `toyread1/write1`（0…16383）。

驱动做三件事：启动时把 `toytrim`/`rtctrim` 写 0（手册要求"软件必须初始化为 0"，该值靠电池保留，
所以每次启动都写）、打开晶振与 TOY 计数（`EO|TEN`，否则时钟不走）、按 `struct rtc_time` 读写 TOY
（`set_time` 把 `tm_nsec/1e8` 夹到 0…9 写进 0.1 s 位；时间字段用 `rtc_utils_validate_rtc_time()` 校验）。

### 板端验证

```text
rtc get rtc0                       # 读当前时间（设备名用 DT label；本树 CONFIG_DEVICE_DT_METADATA=y）
rtc set rtc0 2026-10-06T16:30:00   # 写时间
rtc get rtc0                       # 再读：应与写入一致，且几秒后再读秒数在走
devmem 0x800000001612802c 4        # TOY 低位（MON/DAY/HOUR/MIN/SEC/0.1s）
devmem 0x8000000016128030 4        # TOY 年份（2026 → 0x7e）
devmem 0x8000000016128040 4        # rtcctrl：bit11(TEN) 与 bit8(EO) 应为 1
devmem 0x8000000016000100 4        # 芯片通用配置 0：bit17 conf_rtc_timer_hspeed（复位默认 1）
```

判据：写入后 `rtc get` 返回同一时刻并且秒数继续走；掉电重启后若电池在位，时间应保留（TOY + 电池语义）。

### 已知边界

- **无闹钟**：TOY match（`0x34/0x38/0x3c`）在这颗芯片上不可用，Zephyr 侧未实现 alarm/中断；
  EIOINTC 向量表里给 RTC 的 `RTC-INT0..2 = 62..64`、`RTC-TICK = 68`、`TOY-TICK = 69` 暂时闲置。
- 年份上限被 `rtc_utils_validate_rtc_time()` 限制在 1900…2099（与主线宣称的 2000…2099 一致），
  硬件本身支持到 16383。
- 没有触碰"快速访问"位（芯片通用配置 0 bit17，复位默认 1，关掉会让 RTC 计数器访问变慢）；
  上板若读数异常，先 devmem 核对它。

## 8. SoC 定时器（ATIM/GTIM）：设备树 + PWM 驱动（待硬件验证）

Linux（厂商 6.12 的 `arch/loongarch/boot/dts/loongson-2k0300.dtsi`）把两个通用定时器块写成
`timers1@16118000` / `timers2@16119000`（`loongson,loongson-timers`，即手册第 18/19 章的
**ATIM / GTIM**），**SoC 级本身就是 `disabled`**，由板级 DT 启用并给通道引脚写 pinctrl
（例：`ls2k300_99pi.dtsi` 把它当 LCD 背光 PWM 用）。本端口先把设备树搬了过来，随后在它上面
实现了 PWM 输出：

| 项 | 值 |
|---|---|
| 节点 | `dts/loongarch/loongson/ls2k0300.dtsi` 的 `timers1`（ATIM @0x16118000）、`timers2`（GTIM @0x16119000），各 0x1000，各带一个 `pwm` 子节点（`loongson,ls2k-pwm-timer`，label `atim_pwm` / `gtim_pwm`） |
| 绑定 | 父：`dts/bindings/timer/loongson,loongson-timers.yaml`（含 pinctrl-device）；子：`dts/bindings/pwm/loongson,ls2k-pwm-timer.yaml`（`#pwm-cells = <3>` = channel / period / flags） |
| 驱动 | `drivers/pwm/pwm_loongson_ls_timer.c`（`CONFIG_PWM_LOONGSON_LS_TIMER`，随子节点默认启用）；**寄存器、APB 时钟、pinctrl 全部从父节点取**，与 Linux 的 MFD + child 结构一致 |
| 时钟 | `clocks = <&clk LS2K0300_CLK_APB>`（Linux 的 `CLK_APB`，**APB = 100 MHz**；`pwm_get_cycles_per_sec()` 返回它） |
| 中断 | 走 EIOINTC：ATIM = 向量 25 → flat **41**、GTIM = 向量 26 → **42**（Linux 是传统线 child 13/14，同一硬件源）；PWM 输出路径不用中断 |
| 板级（PAI） | 两块都启用：`&timers1` + `&atim_pwm`（通道 3 → **GPIO83** + 互补输出 CH3N → **GPIO86**，厂商 `atim3_pin_m1`/`atim3n_pin_m1`，Linux 在 99pi 上拿这一对做背光）、`&timers2` + `&gtim_pwm`（通道 2，即厂商表里的 "TIM2_CH2" → **GPIO88**，`gtim2_pin_m1`）；`CONFIG_PWM=y`（板级 defconfig）+ `CONFIG_PWM_SHELL=y`（bringup conf） |
| 设备名 | 子节点 node name 取 `atim-pwm` / `gtim-pwm`（Linux 两个都叫 `pwm`）——Zephyr 的设备名来自 node name，同名无法区分；label 仍是 `atim_pwm` / `gtim_pwm`，两种都能用于 shell |
| 有意没搬 | `dmas`/`dma-names`（只被捕获路径用到，本端口也没有 DMA 控制器节点）、`loongson,breakinput`（ATIM 专有，板级）、互补输出；BTIM（0x1611a000，向量 27 → flat 43）Linux DT 里没有节点 |

移植范围（对照厂商 `drivers/pwm/pwm-ls-timer.c`）：

- **通道数与 ARR 位宽在 init 时探测**（写 CCER 通道使能位 / 写 ARR 全 1 再读回，看哪些位能保留），
  与厂商同法——两个块并不相同（ATIM 有互补输出和 BDTR，GTIM 没有），位宽手册也没写；
- **输出**：`PSC + ARR` 定周期、`CCRx` 定脉宽、CCMR 里 PWM 模式 1 + 输出预装载、CCER 里极性/使能、
  `CR1.ARPE`、`EGR.UG`、`BDTR.MOE`；脉宽 0 → CCRx=0（引脚仍被驱动在无效电平），
  脉宽 = 周期 → CCRx > ARR（常有效电平）——与 PWM API 对 0/满脉宽的约定一致；
- **互补输出（ATIM 专有）**：init 时探测（写 `CC1NE` 看位能否保留），有则配置一个通道时同时使能
  `CCxE | CCxNE`、反相时同时写 `CCxP | CCxNP`（与厂商一致）；**死区不编程**（`BDTR.DTG` 保持 0），
  pin 是否引出由板级 pinctrl 决定（PAI 板：CH3→G83、**CH3N→G86**）；
- **只驱动互补输出**：`CCxE`/`CCxNE` 是两个独立使能位，设备树属性
  `loongson,complementary-only = <0x4>`（按位，bit 0 = 芯片 CH1，故 `0x4` = CH3）让该通道**只**驱动 CHxN
  （清 `CCxE`、置 `CCxNE`），可运行时切换；在没有互补输出的块（GTIM）上设这个属性会让设备初始化失败
  （`-ENOTSUP`），以免静默不出波形；
- **同一块内 PSC/ARR 共用**：另一个通道正在跑且周期不同时返回 `-EBUSY`（厂商同行为），
  不会把已运行的通道悄悄改频；要两个独立频率就同时启用 ATIM 与 GTIM；
- **未实现**：捕获（厂商走 DMA 突发读 + 捕获中断）、break input（`loongson,breakinput`）与死区发生器。

寄存器（手册 18.3，STM32 风格）：CR1 0x00、CR2 0x04、SMCR 0x08、DIER 0x0C、SR 0x10、EGR 0x14、
CCMR1/2 0x18/0x1C、CCER 0x20、CNT 0x24、PSC 0x28、ARR 0x2C、RCR 0x30、CCR1..4 0x34..0x40、
BDTR 0x44；CCMR 每通道一个字节（通道 1/2 在 CCMR1，3/4 在 CCMR2）。

### 板端验证

```text
pwm frequency atim_pwm 3        # 期望 100000000（APB）
pwm usec atim_pwm 3 1000 500    # ATIM 通道 3（GPIO83）：1 kHz、50%
pwm frequency gtim_pwm 1        # GTIM 通道 2（GPIO88）——Zephyr 通道号从 0 起，CH2 = 1
pwm usec gtim_pwm 1 2000 1000   # 500 Hz、50%
```

注意：**必须用 label**（或 node name `atim-pwm`/`gtim-pwm`）。只启用 ATIM 的旧镜像里没有 `gtim_pwm` 这个设备，会出现
"PWM device not found"；同块内第二个通道给不同周期会被驱动以 `-EBUSY` 拒绝（PSC/ARR 共用），要两个频率就用两块。

上面 `usec` 那条之后，寄存器应与算式一致（100 MHz、prescaler=1、ARR=49999 → 周期 1 ms、占空比 50%）：

```text
devmem 0x8000000016118000 4    # CR1：bit0(CEN)=1、bit7(ARPE)=1
devmem 0x800000001611801c 4    # CCMR2：通道 3 的字节 = 0x68（PE + PWM 模式 1）
devmem 0x8000000016118020 4    # CCER：bit8(CC3E)=1、bit10(CC3NE)=1（bit9/bit11 是极性，反相时置 1）
devmem 0x8000000016118028 4    # PSC = 1
devmem 0x800000001611802c 4    # ARR = 49999
devmem 0x800000001611803c 4    # CCR3 = 25000（CCR1 0x34 + 4*2）
devmem 0x8000000016118044 4    # BDTR：bit15(MOE)=1（互补对必须，死区位 DTG 保持 0）
devmem 0x80000000160004a4 4    # G80..G95 复用字：G83(bit6:7)=0b11、G86(bit12:13)=0b11 → 低 16 位 0x30c0
```

只要互补输出时（板级 dts 里给子节点加一行 `&atim_pwm { loongson,complementary-only = <0x4>; };`，
`0x4` = bit 2 = 芯片 CH3）：

```text
pwm usec atim_pwm 2 1000 500   # 波形只出现在 GPIO86（CH3N），GPIO83 不再被驱动（高阻）
devmem 0x8000000016118020 4    # CCER：只有 bit10(CC3NE)=1 → 0x400；bit8(CC3E) 应为 0
```

GTIM 侧（基址 `0x16119000`；**GTIM 没有 BDTR/MOE**，详见下文）：

```text
devmem 0x8000000016119000 4    # CR1：bit0(CEN)=1、bit7(ARPE)=1
devmem 0x8000000016119018 4    # CCMR1：通道 2 在高字节 → 0x6800（低字节属通道 1）
devmem 0x8000000016119020 4    # CCER：bit4(CC2E)=1（bit5 是极性）
devmem 0x8000000016119028 4    # PSC = 1
devmem 0x800000001611902c 4    # ARR = 49999
devmem 0x8000000016119038 4    # CCR2 = 25000（CCR1 0x34 + 4）
devmem 0x80000000160004a4 4    # G88 的两位（bit16:17）= 0b11
```

有示波器/逻辑分析仪时直接量 GPIO83（ATIM/CH3）与 GPIO88（GTIM/CH2）；没有的话，
"寄存器读回与算式一致 + CNT 在走"即驱动侧成立。

**ATIM 与 GTIM 的寄存器差异**（手册表 18-2 对比表 19-2）：GTIM 到 CCR4/INSTA 结束，
**没有 RCR(0x30)、没有 BDTR(0x44)**，也没有互补输出（ATIM1N..3N 是 ATIM 专有）。因此驱动在 init 时
探测 BDTR 是否存在（写 MOE 看位能否保留），只有 ATIM 才写 `MOE`；GTIM 的输出仅由 `CCxE` 门控。
换引脚（`atim3_g30_default` = GPIO30，其他通道见 SoC dtsi 里的表）或换块（`&gtim_pwm`）都只是
板级 dts 的一行改动。

### 下一步（如果还需要别的）

- **counter**：把某个块做成 Zephyr `counter` 设备（输入捕获/脉冲计数）；
- **捕获**：按厂商做 PWM 输入模式，前提是先有 DMA 驱动；
- BTIM（0x1611a000，向量 27 → flat 43）Linux DT 里没有节点，需要时按同样写法补。


## 9. I2S 控制器（本轮新增，待硬件验证）

2K0300 的 I2S（手册第 11 章，厂商叫 IIS）是一路**立体声**接口：每个方向一个 8 字节 FIFO，
一个整数位时钟分频器，加一个分数分频器产生给 codec 的 MCLK。厂商内核把它拆成
`sound/soc/loongson-gd/ls-i2s.c`（DAI）+ `ls-pcm-generic.c`（DMA）+ `ls-sound.c`（声卡），
本端口只保留了控制器本身——**没有 DMA 控制器驱动**，所以走轮询：

| 项 | 值 |
|---|---|
| 节点 | `dts/loongarch/loongson/ls2k0300.dtsi` 的 `i2s0`（label 另有 `i2s_rxtx`，给上游 echo 样例用），`0x16114000` + 0x4000，时钟 `<&clk LS2K0300_CLK_I2S>`（NODE PLL 分出） |
| 绑定 | `dts/bindings/i2s/loongson,ls2k0300-i2s.yaml`（有 `slave-mode`、`mclk-output` 两个开关） |
| 驱动 | `drivers/i2s/i2s_loongson_ls2k0300.c`（`CONFIG_I2S_LOONGSON_LS2K0300`，随节点默认启用），实现 API 的 5 个回调 |
| 寄存器 | `0x0000` Version、`0x0004` Config0（LR_LEN[31:24] TX_DEPTH[23:16] BCLK_RATIO[15:8] RX_DEPTH[7:0]）、`0x0008` Control（MCLK_READY[16] MASTER[15] MSB[14] RX_EN[13] TX_EN[12] RX_DMA_EN[11] CLK_READY[8] TX_DMA_EN[7] RESETn[4] MCLK_EN[3] RX_INT_EN[1] TX_INT_EN[0]）、`0x000c` RxData、`0x0010` TxData、**`0xd014` Config1**（MCLK 整数/分数分频） |
| 时钟公式 | `MCLK = 控制器时钟 / (int + frac/2^16)`，`BCLK = MCLK / (2*(RATIO+1))`；驱动取"让 MCLK ≥ 256×fs 的最小 BCLK 分频"，因此 16 位时 MCLK = 256fs、24 位时 288fs、32 位时 256fs，BCLK 与采样率都精确 |
| 中断 | EIOINTC 向量 24（flat 40）——**不用**：手册的中断语义是"FIFO 满仍写 / 空仍读"的溢出告警，不能当节拍用，故节点没有 `interrupts` |
| 引脚 | G76 MCLK、G77 BCLK、G78 LR、G79 data in、G80 data out（手册引脚表 + 厂商 `i2s_pins`）；G76/G77 另有 ATIM4/GTIM4 功能（f1），板级二选一 |
| 板级（PAI） | `&i2s0` 启用，`pinctrl-0` 只含 BCLK/LR/DI/DO 四个引脚——板子的 ES8388 由板上 **12.288 MHz 固定晶振**供 MCLK，驱动控制器 MCLK 输出会和它抢同一条网络，所以**不设** `mclk-output`；`CONFIG_I2S=y`（板级 defconfig） |

驱动能力与边界：

- **轮询**：没有 DMA，`i2s_write()` 按采样节拍（系统周期计数器 `k_cycle_get_64()`，120 MHz）逐
  "32 位 FIFO 字（= 两帧）"写入，写完缓冲区才返回——**是同步阻塞语义**，不是 DMA 那种排队返回；
  `i2s_read()` 对称（从 slab 取块、按节拍读满）。两个 FIFO 只有 2 个字深，所以**两个方向必须由同一
  线程交错服务**（一个方向空闲太久就会溢出）。
- **立体声 + I2S 格式**（硬件如此）：`channels` 必须为 2、`word_size` 8..32、MSB first、
  非 I2S/左对齐/右对齐/LSB first/loopback 组合一律 `-EINVAL`；两个方向共享格式寄存器，
  所以采样率与字长必须一致（与厂商 `symmetric_rate` 相同）。
- **主/从**由设备树 `slave-mode` 决定；调用方在 `options` 里请求的角色与之不符会 `-EINVAL`
  （无声失败比静默错时序好）。
- **不检测 underrun/overrun**（没有可用的状态位），所以驱动不会进入 `I2S_STATE_ERROR`。
- **没有 codec 驱动**：Zephyr 树内没有 ES8388（见下），所以这个控制器目前只能做寄存器级验证与
  数字环回。

### 板端验证

```text
# 1) 控制器起来了（节点 okay + 驱动 init 会软复位并清中断/DMA 使能）
devmem 0x8000000016114000 4    # IISVersion（厂商/主线都用它做 IP 版本识别）
devmem 0x8000000016114008 4    # IISControl：init 后 bit4(RESETn)=1，bit12/13(TX/RX_EN)=0
devmem 0x80000000160014a4 4    # 引脚复用：G77..G80 的两位应为 0b11（0x30c0_0000 附近值）

# 2) 数字环回：用跳线把 G80(data out) 接到 G79(data in)
west build -b ls2k300_pai -d build/i2s_api tests/drivers/i2s/i2s_api \
      -DEXTRA_CONF_FILE=build/pai_bringup_shell.conf
```

上游两个 I2S 载体的**实际状态**（都实测过构建/编译，别照文档想当然）：

- `samples/drivers/i2s/echo`：**在本树里编译不过**（上游样例落后于 API：`main.c:344`
  `i2s_read` 的第三个参数类型不匹配 `-Wincompatible-pointer-types`）。要用得先修样例。
- `tests/drivers/i2s/i2s_api`：**能编译**并且能找到我们的设备（`depends_on: i2s`），但运行时需要
  两件东西：① DT 别名 `i2s-node0 = &i2s0;`（测试用 `DT_ALIAS(i2s_node0)` 取设备，缺它时
  `DEVICE_DT_GET_OR_NULL()` 得到 NULL，`zassert_not_null` 会直接失败）；② 环回——默认测试用例给
  `i2s_configure()` 加 `I2S_OPT_LOOPBACK`，而本 IP 的寄存器里**没有环回位**（驱动会 `-EINVAL`），
  所以要走 `CONFIG_I2S_TEST_USE_GPIO_LOOPBACK=y` + 外部跳线（或自己写个小应用）。
  DT 别名可以用一行 overlay 加：`/ { aliases { i2s-node0 = &i2s0; }; };`

以 48 kHz/16 位 `i2s_configure()` 之后，寄存器应该是：

```text
devmem 0x8000000016114004 4    # Config0 = LR_LEN(16)<<24 | TX_DEPTH(16)<<16 | BCLK_RATIO(3)<<8 | RX_DEPTH(16)
                               #        = 0x10100310
devmem 0x8000000016114008 4    # Control = RESETn(bit4)|MSB(bit14)|MASTER(bit15) = 0xc010；START 后加 bit12/13
```

### ES8388 codec：Zephyr 不支持（结论）

- 树内**没有** ES8388（也没有 ES8311/ES8328/ES8326 等 Everest 器件）：全仓搜索 `es8388` 只命中
  本文件的说明；`drivers/audio/` 现有 codec 是 aw88298 / cs43l22 / da7212 / max98091 / mpxxdtyy /
  pcm1681 / tas6422dac / tlv320aic3110 / tlv320dac310x / wm8904 / wm8962 / sf32lb。
- 好消息：**vendor prefix 已注册**（`dts/bindings/vendor-prefixes.txt` 里有
  `everest  Everest Semiconductor Co. Ltd.`），所以绑定可以直接写 `compatible = "everest,es8388"`。
- 板子上的位置：**i2c2 @0x11**（厂商 6.12 的 `ls2k300_vanguard_pi_common.dtsi`：
  `codec: es8388@11 { compatible = "everest,es8388"; ... clocks = <&es8388_clk 0>; }`，
  固定 12.288 MHz），SoC 侧是 I2S **主**（`SND_SOC_DAIFMT_CBS_CFS`），codec 做从。
- 要移植的话参考主线 `sound/soc/codecs/es8328.c` + `es8328-i2c.c`（**两家内核里 ES8388 都归这个
  驱动管**：`es8328-i2c.c` 的 of_match 里就有 `everest,es8388`），Zephyr 侧落成
  `drivers/audio/es8388.c`（实现 `include/zephyr/audio/codec.h` 的 codec API，走 I2C——本端口的
  I2C 驱动已有），绑定 `dts/bindings/audio/everest,es8388.yaml`，并把 codec 挂到 I2S 节点下
  （Zephyr 的 I2S codec 是 `i2s-device` 子节点）。

## 10. 复现配方

```bash
# 1) 显示示例（SSD1306）
west build -p always -b ls2k300_pai -d build/display samples/drivers/display \
  -DEXTRA_CONF_FILE=/home/kyoko/zephyr/build/pai_bringup_shell.conf

# 2) LVGL（单色）
west build -p always -b ls2k300_pai -d build/lvgl samples/subsys/display/lvgl \
  -DEXTRA_CONF_FILE="/home/kyoko/zephyr/build/pai_bringup_shell.conf;/home/kyoko/zephyr/build/pai_lvgl.conf"

# 3) 部署（TFTP 根目录）
cp build/<name>/zephyr/zephyr.uImage /mnt/h/Zephyr/zephyr.uImage   # 活动镜像
cp build/<name>/zephyr/zephyr.uImage /mnt/h/Zephyr/zephyr_<name>.uImage  # 留档
```

板上验证顺序：`help` → `i2c scan i2c1`（应看到 `0x3c`）→ `i2c speed i2c1 2` →
屏幕表现（示例/ LVGL）→ 需要时 `devmem` 读 CR1/CR2/CCR/SR1 核对寄存器。
