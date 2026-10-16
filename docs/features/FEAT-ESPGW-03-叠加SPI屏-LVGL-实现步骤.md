# FEAT-ESPGW-03：叠加 SPI 显示屏（ST7735S）+ LVGL —— 实现步骤

> 状态：**方案与实现步骤（⛔ 未实施；排 RS485 收官后开工）**
> 日期：2026-10-09
> 前置：**FEAT-ESPGW-01**（主站轮询 + 数据字典 + 串口打印）已交付；**FEAT-ESPGW-02**（WiFi STA + MQTT 上行）已交付
> 编制：**审查官**（只读审查；本文档为方案/步骤，⛔ 未改任何源码/配置/固件/接线）
> 依据：**R27 三源原则**（①读被调函数源码 ②Issue 区 ＋ **权威社区 R33** ③坑清单）＋ **R32（来源须可点击）**
> 排期：**RS485 主站收官后启动**（用户指令："等 rs485 收官一起做"）
> 桌面证据归档（审查官产出，不在本工程）：`桌面\报告\审计报告\esp\`
> - ① `…方案评审-叠加SPI显示屏-2026-10-09.md`（**v3**）② `…LVGL落地方案与显示内容清单-2026-10-09.md`
> - ③ `…叠加SPI屏-LVGL实施方案-2026-10-09.md` ④ `…叠加SPI屏-三源核查与坑清单-2026-10-09.md`（**v2，坑 P-01~P-12**）
>
> **⇒ 本文档 = 本工程内的「实现步骤」单一事实源；①②③④ 为证据归档与细节来源。**

---

## 0. 目标与范围

在已交付的 01（Modbus RTU 主站轮询）/ 02（MQTT 上行）之上，**叠加一块 1.8 寸 SPI TFT 显示屏**，用于**本地可视化**：主站/网络/系统状态 + 从站 5×Holding 快照。

- **屏**：1.8 寸 TFT，驱动 IC **`ST7735S`**，分辨率 **128×160**，RGB565，4-line SPI（只写）
- **软件栈**：`waveshare/esp_lcd_st7735`（面板驱动）→ `espressif/esp_lvgl_port` v2.x（移植层）→ `lvgl/lvgl` 9（控件/字体）
- **不改动**：主站 init 序列、数据字典、轮询周期、RS485 半双工配置
- **显示两态**：**态 A**（现在即可信：主站/网络/系统）与 **态 B**（从站 5×Holding，RS485 通后）

> **派生结论（预期）：03 = 纯增量（新增 1 个外设 + 1 个任务），不改 01/02 的核心。**

---

## 1. 来源（R32 可溯回链）

| 层 | 来源 | 路径 / 链接 |
|---|---|---|
| **芯片权威源** | ST7735S Datasheet V1.1（Sitronix） | `桌面\stm32F407ZGT6\ST7735S_datasheet.pdf` / `.txt` |
| **实物权威源** | 同款屏在跑的项目 `ov-watch`（驱动 + 连接文档） | `C:\Projects\ov-watch`：`docs\hardware-connections.md:9-51`、`firmware\Core\Inc\pin_config.h:12-43`、`firmware\Drivers\BSP\lcd\st7735.c:32-86` |
| **面板驱动组件** | `waveshare/esp_lcd_st7735`（导出 `esp_lcd_new_panel_st7735()`） | https://components.espressif.com/components/waveshare/esp_lcd_st7735 |
| **LVGL 移植层** | `espressif/esp_lvgl_port` v2.x | https://components.espressif.com/components/espressif/esp_lvgl_port |
| **IDF 本地头文件（①源）** | `esp_lcd_panel_io_spi.c:65-115`、`esp_lcd_panel_ops.c:13-73` | `components/esp_lcd/…`（IDF v5.4） |
| **Issue 区（②源前半）** | esp-bsp **#716**（`trans_size`+双缓冲 ⇒ `flush_ready` 提前）、**#707**（缓冲不可运行时切换） | https://github.com/espressif/esp-bsp/issues/716 |
| **权威社区（②源后半，R33）** | StackOverflow [#79141161](https://stackoverflow.com/questions/79141161/esp32-s3-geek-lcd-screen-programming)；esp32.com [t=18957](https://esp32.com/viewtopic.php?t=18957) / [f=19&t=44897](https://esp32.com/viewtopic.php?f=19&t=44897) / [t=3029](https://esp32.com/viewtopic.php?t=3029)；forum.lvgl.io [#7807](https://forum.lvgl.io/t/esp-s3-issue-with-horizontal-banding-on-lcd-ili9488/7807/6) / [#12109](https://forum.lvgl.io/t/ili9488-display-issues-using-esp32s3-parallel-3-5-touch/12109/26) / [#9049](https://forum.lvgl.io/t/periodic-wrong-pixels-using-dma-spi-esp32/9049) / [#16849](https://forum.lvgl.io/t/esp32-s3-st7789-lvgl-platformio-esp-idf-spi/16849) | 见④ §2.5 |
| **数据来源** | `holding_regs` 快照 + 状态字段 | `main\mb_param.h:51-57`、`main\master_app.c` |

> **⚠️ 前提已核实**：本工程 `grep -rc "esp_lcd_\|lvgl_port_\|lv_" main/*.c main/*.h` ⇒ **全 0 命中** ⇒ 上述 API 均为"项目从未用过" ⇒ **R27 生效**。

---

## 2. 关键参数（DR 决策）

| 项 | 取值 | 依据 |
|---|---|---|
| 面板驱动 IC | **`ST7735S`**（非 ST7789） | 芯片手册 + ov-watch 三处一致 |
| 分辨率 | **128×160** | 同上 |
| SPI 时钟 | **10 MHz**（范围 9~10.5；**规格上限 15.15 MHz**） | `TSCYCW ≥ 66 ns` + ov-watch 实跑 |
| SPI 模式 | **mode 0**（CPOL0/CPHA0） | 芯片手册 |
| 半双工 | `SPI_DEVICE_HALFDUPLEX`（`esp_lcd_new_panel_io_spi` 内部走此模式） | IDF `esp_lcd_panel_io_spi.c:77` |
| `MISO` | **不接**（`miso_io_num = -1`） | 该模组不回读 |
| GRAM 偏移 | **`x_gap=2 / y_gap=1`**（GREENTAB） | 手册（132×162 vs 128×160）+ ov-watch + 第三方 **3/3 一致** |
| `MADCTL` | **`0xC8`** | ov-watch `st7735.c:74` |
| `COLMOD` | **`0x05`**（16-bit RGB565） | ov-watch `st7735.c:71` |
| 反色 | **先 `INVOFF`（invert=false）**；**负片再改 true** | 手册默认 Off + ov-watch 用 Off（**waveshare 用 On ⇒ 模组相关，必须实测**，P-02） |
| 颜色顺序 | **`LCD_RGB_ELEMENT_ORDER_BGR`** | ov-watch 实测 |
| DC 极性 | **默认低=命令 / 高=数据**（`flags` 保持全 0） | IDF `esp_lcd_panel_io_spi.c:102-104` |
| CS | **必须接**（该模组有 CS），交 **SPI 外设**驱动 | `esp_lcd_panel_io_spi.c:83`；esp32.com t=3029 |
| 引脚（**建议，实物丝印待实测**） | `SCLK=GPIO12` / `MOSI=GPIO11`（SPI2 IOMUX 默认）、`CS=GPIO5`、`DC=GPIO9`、`RST=GPIO14`、`BL=GPIO4` | `master_app.c:167-177`（GPIO10 已被 Modbus RTS 占用，故 CS 避开 GPIO10）；⚠️ 见 §7 待实测 |
| 绘制缓冲 | 整屏 **128×160×2 = 40 KB**（内部 RAM）**＋ 单缓冲**（`double_buffer=false`） | 规避 esp-bsp #716 / forum.lvgl.io #7807（P-03/P-08） |
| 分区表 | **`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`**（factory **1 M → 1500 K**） | IDF `partitions_singleapp_large.csv`（当前 `sdkconfig:545` 为 `SINGLE_APP`） |

### 2.1 相对官方例程 / 其他实现的裁剪决策（DR 记录）

| # | 参照实现 | 本项目 | 理由 |
|---|---|---|---|
| C-21 | 官方例程用 `esp_lcd_new_panel_st7789()` | **换** `esp_lcd_new_panel_st7735()` | IDF v5.4 **不带** ST7735 驱动（`grep -ril st7735` 0 命中） |
| C-22 | 常见例程调 `invert_color(panel, true)` | **先 `false`** | 同款屏 ov-watch 发 `INVOFF`；芯片手册默认 Off（P-02） |
| C-23 | 常见例程 `ESP_ERROR_CHECK(set_gap(...))` | **改：检查返回值 + `init_cmds` 兜底** | `set_gap` 未被组件实现时返回 `ESP_ERR_NOT_SUPPORTED`，`ESP_ERROR_CHECK` 会 **abort**（P-01） |
| C-24 | 常见例程用大缓冲/双缓冲 | **整屏单缓冲 40 KB** | esp-bsp #716 + forum.lvgl.io #7807（P-03/P-08） |
| C-25 | 官方 `UPDATE_CIDS_TIMEOUT_MS=500` 快轮询 | **不动**：沿用 01 的 **1000 ms** 周期 | GW-05；03 不得改轮询时基 |
| C-26 | 例程直连 27 MHz（esp32.com t=2250） | **不采信**，取 **10 MHz** | 27 MHz **超本屏规格**（上限 15.15 MHz） |

---

## 3. 实现步骤（S0 ~ S6）

> 每步 = **目标 / 动作 / 产出 / 验收 / 回退**。**S1~S2 在独立工程**；**S3 起并入主工程**。

### S0 前置（RS485 收官后）

| | |
|---|---|
| **目标** | 让"改构建配置"这件事不再干扰任何未闭合的排查 |
| **动作** | ① 确认 RS485 **②-f 四路同抓 + D1** 已完成并有结论；② 清掉 `flash ≠ build`（`Checksum mismatch`）；③ `idf.py fullclean`；④ **git 提交一次干净基线**（打 tag） |
| **产出** | 可回退的基线提交 |
| **验收** | `git status` 干净；`app_init: Project name:` 与当前 `.elf` 一致（T27） |
| **回退** | `git reset --hard <tag>` |

### S1 独立工程 + 分区表 + 依赖

| | |
|---|---|
| **目标** | 建一个**不碰主工程**的屏驱动验证工程 |
| **动作** | ① 新建 `C:\Projects\lcd_st7735_probe\`（或 `idf.py create-project`）；② 分区表切 **`SINGLE_APP_LARGE`**（factory 1500 K）；③ 加依赖（见下）；④ `main/CMakeLists.txt` 的 `REQUIRES` 加 `esp_lcd esp_lcd_st7735`；⑤ 🔴 **补 R27 ①源**：依赖拉下来后立刻 `grep -n "set_gap\|invert_color\|draw_bitmap" managed_components/esp_lcd_st7735/*.c` ⇒ **确认 `panel->set_gap` 是否实现**（决定 P-01 走哪条路，并回补④） |
| **依赖写法** | `dependencies:` → `idf: ">=5.0"` + `esp_lcd_st7735: "^2"`（先 `idf.py add-dependency` 让管理器解析实际版本） |
| **产出** | 可 `idf.py build` 的空工程 |
| **验收** | 编译通过；`idf.py size` 见到分区余量 |
| **回退** | 删目录 |

### S2 点亮屏（**关键段**）—— 只做 4 件事

| | |
|---|---|
| **目标** | 屏幕出**纯色 + 四边框**，并把 4 个变量定死 |
| **代码骨架** | 见 §4.1（已按 IDF v5.4 头文件逐字段核对） |
| **交付 4 个定值** | ① `x_gap/y_gap`（先试 **2/1**）② 颜色顺序（**BGR**）③ 是否需 `invert_color` ④ **背光极性**（先高电平试；不亮试低） |
| **验收** | 纯红铺满**无白边**、边框四边**不漏行**；红/绿/蓝分别正确（判 R2/R3） |
| **回退** | 逐项二分：**只改一个变量再测** |

**🩺 S2 现场排查卡（症状 → 处置）**：

> **⚠️ 顺序铁律（P-10）**：**先查"接线 / 供电"，再改代码参数** —— 社区实测"白屏"绝大多数是接线问题，**不是**驱动参数问题。

| 现象 | 处置 | 依据 |
|---|---|---|
| **全白**（背光亮、无图） | 🔴 **先**：①量 `BL` 是否亮（亮=供电/背光 OK，问题在数据链）②查 **`RST`/`DC` 接线**与 **`RST` 电平**（**ESP32-S3 上 `RST` 未拉高即白屏**）③**缩短连线 / 换板**、确认共地；**最后**才改参数 | esp32.com t=18957 / f=19&t=44897（**P-10**） |
| **全黑**（无背光） | 查 `BL` 驱动方式（ov-watch = **TIM3 CH3 PWM**，**不是**普通 GPIO 拉高）/ 供电 | ov-watch `pin_config.h` |
| **负片**（黑变白、整体反色） | `esp_lcd_panel_invert_color(panel, true)` | 手册默认 Off / ov-watch Off / waveshare On ⇒ **模组相关，逐块定**（P-02） |
| **红蓝反**（绿正常） | `rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR`；再看 `flags.swap_bytes` / `data_endian` | tinygo ST7735 文档 |
| **雪花/噪点、整体错位** | 调 `set_gap(panel, x, y)`（先 2/1，左右偏调 x、上下偏调 y） | 手册 132×162 vs 128×160 |
| **横向条带 / 隔块丢传** | ⛔ 别用 **≥40 行**条带缓冲 ⇒ **降到 ≤20 行**；本方案用**整屏单缓冲**天然规避 | forum.lvgl.io #7807（**P-08**） |
| **帧数不足/撕裂** | 降 `pclk_hz`（10 MHz → 8 MHz） | `TSCYCW ≥ 66 ns` |
| **`set_gap` 返回 `ESP_ERR_NOT_SUPPORTED`** | 走 `vendor_config.init_cmds` 自设 CASET/RASET 起址 | **P-01** |
| **全黑但 SPI 无报错** | 查背光极性 / `disp_on_off(true)` 是否执行 | — |

### S3 接入 LVGL（**并入主工程**）

| | |
|---|---|
| **目标** | 屏上稳定显示**一个静态文字**，LVGL 任务与轮询任务并存不打架 |
| **动作** | ① 加依赖 `lvgl/lvgl`（9.x）+ `espressif/esp_lvgl_port`（`^2`）；② `idf.py menuconfig`：勾 Montserrat 14/20，关掉用不上的模块；③ 写 `display_port.c`（§4.2）；④ 建 LVGL 任务跑 `lv_timer_handler`；⑤ 🔴 **核任务优先级**：`modbus_poll_task` 优先级 = **1**（`master_app.c:210`）⇒ **LVGL 任务不得高于它** |
| **产出** | 屏上出现 `Hello`；串口仍稳定打印 CID#0..4 |
| **验收** | 连续 5 min：`transaction processing time` 仍 **1000/1040 ms 交替**；无 watchdog |
| **回退** | 去掉 `esp_lvgl_port` 依赖即回 S2 |

### S4 态 A 界面（**现在就可信**）

| | |
|---|---|
| **目标** | 把 A 类字段搬上屏：`轮询计数` / `round_ok 0/5` / `最近事务 ms` / `错误码` / `WiFi·IP·MQTT` / `运行时长` / `free heap` |
| **动作** | ① Modbus 任务**只写**一个 `gateway_status_t` 快照；② 经 **FreeRTOS 队列** 送显示任务；③ 显示任务内 `lvgl_port_lock(0)` 后刷 label；④ **1 s 刷一次**（别每帧刷） |
| **验收** | 拔掉从站 ⇒ 屏上 `round_ok` 显 **0/5**、5 寄存器显 **`--`**（**不得显 0**） |
| **回退** | 保留 S3 |

### S5 态 B 界面（**从站数据，RS485 通后才做**）

| | |
|---|---|
| **目标** | 显示 SoundDog 5×Holding + 报警横幅 |
| **动作** | ① 复用 `holding_regs` 快照（`mb_param.h:51-57`）；② 报警状态机：`ALARM_CNT`/`RELEASE_CNT` 推 正常/累积中/**报警**；③ **报警用红色** |
| **验收** | 从站应答时，屏上 5 值与 MQTT payload / 串口打印**三方一致** |
| **回退** | 保留 S4 |

### S6 压测与验收

| | |
|---|---|
| **动作** | 连续跑 **≥10 min**：看轮询时基、`EV_FRAME_RECEIVED` 计数、`free heap`（含最小值）、LVGL perf monitor（可选） |
| **验收** | 见 §5 |

---

## 4. 关键代码骨架

### 4.1 `esp_lcd` 点亮屏（字段名已对 IDF v5.4 头文件核对）

```c
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"      /* 类型 */
#include "esp_lcd_io_spi.h"        /* esp_lcd_new_panel_io_spi 声明处 */
#include "esp_lcd_panel_ops.h"     /* set_gap / invert_color / disp_on_off  ← P-09：必须 include */
#include "esp_lcd_panel_dev.h"     /* esp_lcd_panel_dev_config_t */
#include "esp_lcd_st7735.h"        /* waveshare 组件 */

#define LCD_H 128
#define LCD_V 160
#define PIN_SCLK 12  /* SPI2 IOMUX 默认 */
#define PIN_MOSI 11
#define PIN_CS    5  /* J1 pin5（GPIO10 被 Modbus RTS 占用） */
#define PIN_DC    9
#define PIN_RST  14
#define PIN_BL    4
/* MISO 不接：miso_io_num = -1（该模组不回读） */

void lcd_probe(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H * LCD_V * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC,
        .spi_mode = 0,                       /* ST7735S = CPOL0/CPHA0 */
        .pclk_hz = 10 * 1000 * 1000,         /* 9~10.5 MHz；上限 15.15 MHz */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,  /* v5.4：color_space 已 deprecated */
        .bits_per_pixel = 16,
        /* .data_endian = LCD_RGB_DATA_ENDIAN_BIG, */  /* 字节序不对时才加 */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io, &dev_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* ⚠️ P-01：set_gap 是"转发到面板驱动"的，该组件若未实现会返回
     * ESP_ERR_NOT_SUPPORTED（esp_lcd_panel_ops.c:54-59）
     * ⇒ 绝不能用 ESP_ERROR_CHECK 包住（会直接 abort）。 */
    esp_err_t e = esp_lcd_panel_set_gap(panel, 2, 1);   /* GREENTAB：col+2 / row+1 */
    if (e != ESP_OK) {
        ESP_LOGW("LCD", "set_gap -> 0x%x（组件未实现），改走 vendor_config.init_cmds 兜底", e);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));  /* 先按 ov-watch/手册；负片→改 true（P-02） */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}
```

> 📌 **字段出处（本地 IDF v5.4）**：`esp_lcd_io_spi.h:22-44`（io 结构体）、`:57`（`esp_lcd_new_panel_io_spi`）、`esp_lcd_panel_dev.h:20-33`（dev 结构体）、`esp_lcd_panel_ops.h:101/111/122`（`set_gap`/`invert_color`/`disp_on_off`）、`esp_lcd_types.h:38-41`（`LCD_RGB_ELEMENT_ORDER_BGR`）。

### 4.2 接 LVGL（`esp_lvgl_port`）

```c
#include "esp_lvgl_port.h"

void ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* ⚠️ modbus_poll_task 优先级=1（master_app.c:210）⇒ LVGL 不得更高 */
    /* port_cfg.task_priority = 1; */
    /* port_cfg.task_max_sleep_ms = 500; */
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .hres = LCD_H, .vres = LCD_V,
        .monochrome = false, .mipi_dsi = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .buffer_size  = LCD_H * LCD_V,      /* 单位=像素；40 KB 内部 RAM */
        .double_buffer = false,             /* P-03/P-08：不开双缓冲 */
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = false },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

    /* ⚠️ 任何跨任务操作 UI，必须加锁 */
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_disp_get_scr_act(disp);
        lv_obj_t *lb  = lv_label_create(scr);
        lv_label_set_text(lb, "Hello");
        lvgl_port_unlock();
    }
}
```

### 4.3 主工程要改的三处（**RS485 冻结后才动**）

| 文件 | 改什么 |
|---|---|
| `main\idf_component.yml` | 加 `lvgl/lvgl`、`espressif/esp_lvgl_port`、`esp_lcd_st7735` |
| `main\CMakeLists.txt` | `PRIV_REQUIRES` 追加 `esp_lcd esp_lvgl_port lvgl esp_lcd_st7735` |
| `sdkconfig` | `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` |

---

## 5. 验收清单（AC）

| # | 验收项 | 判据 |
|---|---|---|
| AC-1 | 屏点亮、纯色无白边、无错位 | S2 |
| AC-2 | 红/绿/蓝三色正确（判颜色顺序与反色） | S2 |
| AC-3 | **轮询时基不变** | 连续 ≥10 min 仍 **1000/1040 ms 交替** |
| AC-4 | **从站计数不变** | `EV_FRAME_RECEIVED` 计数逻辑照常（当前 0 也应保持 0） |
| AC-5 | **内存不泄漏** | `free heap` 最小值 ≥ 基线 − 40 KB，且 10 min 内不持续下滑 |
| AC-6 | **离线显示诚实** | 从站离线 ⇒ `0/5` + `--`，**无绿灯** |
| AC-7 | **三方一致** | 从站在线时，屏 / 串口 / MQTT payload 数值一致 |
| AC-8 | 0 error 0 warning 编译 | `idf.py build` |
| AC-9 | 可回退 | `git reset --hard <基线tag>` 能回到 S0 |

---

## 6. 坑清单联动（本 FEAT 命中，来自④）

| 坑 | 级别 | 落地 |
|---|---|---|
| **P-01** | 🔴🔴 | `set_gap` 可能返回 `ESP_ERR_NOT_SUPPORTED` ⇒ **禁 `ESP_ERROR_CHECK` 包装**，改查返回值 + `init_cmds` 兜底（§4.1 已落） |
| **P-02** | 🟠 | 反色**模组相关** ⇒ 先 `false`，负片改 `true`（S2 4 个定值之一） |
| **P-03** | 🔴 | `trans_size` 分块 + 双缓冲 ⇒ `flush_ready` 提前 ⇒ **本方案单缓冲整屏规避** |
| **P-04** | 🟢 | 偏移 `2/1` 三源闭环，S2 仍要复核边框 |
| **P-05** | 🟠 | LVGL 9 较吃内存 ⇒ 缓冲**从小起步**、对象复用 |
| **P-06** | 🟠 | `trans_queue_depth=10`；CS 交 SPI 外设；DC 默认低=命令 ⇒ **`flags` 全 0** |
| **P-07** | 🟠 | 实际 SPI 时钟 ≤ 请求值（分频取整），本屏余量充足 |
| **P-08** | 🔴 | **ESP32-S3** 上 ≥40 行条带缓冲会**隔块丢传** ⇒ 不用条带；若用则 ≤20 行 |
| **P-09** | 🟠 | 骨架**必含 `esp_lcd_panel_ops.h`**（§4.1 已含） |
| **P-10** | 🔴 | "白屏"**首因是接线 / 线长 / `RST`·`DC`** ⇒ 排查卡**置于改参数之前** |
| **P-11** | 🟠 | 若自写 flush：DMA 缓冲 **init 一次性分配 + 32-bit 对齐 + 每帧前确认上帧完成** |
| **P-12** | 🟢 | 参数旁证：10 MHz / HALFDUPLEX / queue=10 与社区一致 |

---

## 7. 绝对不动 / 待实测 / 边界

**绝对不动（保障 01/02 成果）**：
- 主站 init 序列：`mbc_master_create_serial → set_parameter → uart_set_pin → set_descriptor → uart_set_mode(RS485_HALF_DUPLEX) → start`
- 数据字典：0 基址、范围宏、5×Holding 映射（GW-01/02/07/10）
- 唯一轮询任务：独占主站 API、**1000 ms** 周期（GW-04/GW-05）
- RS485 半双工：RTS 自动驱动 DE/RE

**🟠 待实测（不得写成已知）**：
1. **1.8 寸模组丝印**：引脚编号顺序、是否引出 `SDO`、背光是否需外部驱动管 ⇒ **须看实物**
2. §2 的**引脚分配**（12/11/5/9/14/4）为**建议值**，须与实物丝印 + 空闲脚台账复核
3. `esp_lcd_st7735` / `esp_lvgl_port` **组件源码不在本机** ⇒ **S1 下载后必须补读**（补 R27 ①源）

**边界声明**：本文档由**审查官**编制，为**方案与步骤**；⛔ 未改 `main\`、`sdkconfig`、分区表、`idf_component.yml`、任何固件与接线；⛔ 未改 `C:\Projects\ov-watch`（只读）。**是否采纳、何时开工，由用户决定。**
