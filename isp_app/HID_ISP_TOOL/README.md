# HID_ISP_TOOL

用于 Nuvoton **M3331** HID ISP Bootloader 的上位机（PC 端）烧录工具，搭配
[`ISP_HID_20`](../../SampleCode/ISP/ISP_HID_20) 韧体使用。工具透过
**Windows 原生 HID 驱动程式**（SetupAPI + `hid.dll`）以 64 位元组 HID 报告与装置
通讯，从 `.bin` 映像档烧录应用程式韧体（APROM）。由于使用系统内建的 HID 驱动，
**无需**以 Zadig 更换成 WinUSB 驱动程式。

本工具以 CMake 管理，并对齐 `ISP_HID_20` 命令协定。

## 功能

- 自动侦测目标通道（LDROM Bootloader `0x3F00` 或 CFU 应用 `0xF502`）
- 连线至装置并同步封包序号（Packet Number）
- 读取 Device ID（`SYS->PDID`）、韧体版本、`CONFIG0/CONFIG1`、开机模式
  - LDROM Bootloader：`GET_FWVER` 回传 1 位元组 Bootloader 版本（`0x32`）
  - CFU 应用通道：`GET_FWVER` 回传 4 位元组 **APROM 韧体版本** `g_FwVersion`
- 从 `.bin` 档烧录 APROM（仅限 LDROM Bootloader 通道）
- 将装置回报的 Flash 校验和（checksum）与上位机自行计算的校验和比对验证
- 烧录成功后跳转至应用程式（`RUN_APROM`）——LDROM Bootloader 收到此命令会重映射向量页至选定的 AP Bank 后重启，直接开机进 APROM
- 于 CFU 应用通道以 `--run-ldrom` 送出 `RUN_LDROM`，切换装置进入 LDROM Bootloader

工具会**自动侦测**目标装置并选择对应通道，无需手动指定：

- **LDROM ISP Bootloader**（PID `0x3F00`，无 Report ID 的原始 64 位元组封包）——支援读取资讯**与烧录 APROM**。
- **CFU 应用 HID ISP 通道**（[`HSUSBD_HID_CFU`](../../SampleCode/StdDriver/HSUSBD_HID_CFU)，PID `0xF502`，封包前置 Report ID `40` / 读取 `41`）——支援读取资讯（含 APROM 韧体版本）与 `--run-ldrom` 切换开机，但**不做** APROM 烧录。

详见下方 [与 CFU 应用的关系](#与-cfu-应用的关系)。

## 目录结构

```
HID_ISP_TOOL/
  CMakeLists.txt
  README.md
  src/
    main.c          命令列解析与更新流程
    isp_proto.c/.h  ISP 命令层（64 位元组封包协定）
    usb_hid.c/.h    Windows 原生 HID 传输层（开启/关闭、收发 64 位元组）
```

## 编译（Windows，MSVC x64）

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

可执行档会输出至 `build/Release/HID_ISP_TOOL.exe`。工具连结系统内建的
`hid.lib` 与 `setupapi.lib`，无须额外的执行期 DLL。

> 也可从仓库根目录执行 [`build_all.bat`](../../build_all.bat) 一并编译韧体（ISP_HID_20、
> HSUSBD_HID_CFU AP0/AP1）与本工具，产物会收集到根目录的 `out\`（含 `HID_ISP_TOOL.exe`）。
> 该流程需要 `cmake` 与 MSVC 工具链在 PATH 上（建议从 Visual Studio Developer Command Prompt 执行）。

## 使用方式

```powershell
# 烧录 APROM、验证校验和，然后执行应用程式（自动侦测通道）
HID_ISP_TOOL.exe firmware.bin

# 烧录并验证，但停留在 Bootloader
HID_ISP_TOOL.exe firmware.bin --no-run

# 仅读取装置资讯（不烧录）；CFU 应用通道会显示 APROM 韧体版本
HID_ISP_TOOL.exe --info

# 装置正跑 CFU 应用时，先切换进 LDROM Bootloader，再重跑烧录
HID_ISP_TOOL.exe --run-ldrom

# 将整颗 APROM（256 KB，含 AP0 + AP1 + Calibration + OEM）回读存成 .bin
# 仅 LDROM Bootloader 通道支援；若装置在 CFU 应用，请先 --run-ldrom
HID_ISP_TOOL.exe --dump aprom_backup.bin

# 强制指定通道（一般不需要，预设自动侦测）
HID_ISP_TOOL.exe --info --ldrom   # 仅连 PID 0x3F00
HID_ISP_TOOL.exe --info --cfu     # 仅连 PID 0xF502（Report ID 40/41）

# 说明
HID_ISP_TOOL.exe --help
```

装置以标准 HID 装置列举，由 Windows HID 类别驱动程式接管，因此**不需要**
安装或更换任何驱动程式即可直接使用。

## 协定摘要

64 位元组 HID 封包。两个通道的差异仅在于 **Report ID** 与传输层的报告长度，命令层布局相同：

- **LDROM Bootloader**：无 Report ID（Windows 报告缓冲区前置 `0x00`），传输层自动附加与移除。
- **CFU 应用通道**：OUT 报告前置 Report ID `40`、IN 报告为 `41`，由 `usb_hid.c` 依 `hid_set_report_ids()` 设定；报告长度由 `HidP_GetCaps` 自动查询。

| 栏位 | 请求（Request） | 回应（Response） |
|-------|---------|----------|
| `[0..3]`  | 命令码（command opcode） | 请求封包的 16 位元总和（`[0..1]`） |
| `[4..7]`  | 封包序号               | 封包序号 + 1 |
| `[8..11]` | 位址                   | 回应资料 |
| `[12..15]`| 长度                   | 回应资料 |
| `[16..63]`| 资料                   | 回应资料 |

每次命令被确认后，上位机会将封包序号加 2。

使用到的命令（参见 `src/isp_proto.h`）：`CONNECT 0xAE`、`SYNC_PACKNO 0xA4`、
`GET_DEVICEID 0xB1`、`GET_FWVER 0xA6`、`READ_CONFIG 0xA2`、`GET_FLASHMODE 0xCA`、
`UPDATE_APROM 0xA0`、`RUN_APROM 0xAB`、`RUN_LDROM 0xAC`。

**APROM 更新流程：** 第一个封包携带命令、起始位址、总长度与前 48 个资料位元组；
装置会在确认前先擦除 APROM。之后每个封包携带 56 个资料位元组。最后一个确认封包
会回传 16 位元 Flash 校验和，工具会将其与自行对映像档计算的结果比对。

## 与 CFU 应用的关系

本仓库有**两个**会讲 HID ISP 协定的目标，两者身分与封包格式不同，本工具皆已支援并自动侦测：

| 目标 | 程式位置 | VID / PID | Report ID | 支援功能 |
|------|---------|:---------:|:---------:|---------|
| LDROM ISP Bootloader（[`ISP_HID_20`](../../SampleCode/ISP/ISP_HID_20)）| LDROM | `0x0416` / `0x3F00` | 无（原始 64 位元组）| 读资讯 + **烧录 APROM** + **`--dump` 回读 APROM** |
| CFU 应用 HID ISP 通道（[`HSUSBD_HID_CFU`](../../SampleCode/StdDriver/HSUSBD_HID_CFU)）| APROM | `0x0416` / `0xF502` | Output `40` / Input `41` | 读资讯 + `--run-ldrom` 切换 |

CFU 应用在同一个 HID 介面上同时承载 CFU 升级协定与 ISP 通道，藉由 **Report ID**（封包第 0 位元组）区分。
其 ISP 通道仅提供**资讯查询与开机切换**（读版本、Device ID、Flash 模式、`RUN_LDROM` / `RUN_APROM`），
**不做** APROM 烧录。因此当装置正跑 CFU 应用、需要更新 APROM 时，典型流程为：

1. `HID_ISP_TOOL.exe --run-ldrom` — 透过 CFU 通道送出 `RUN_LDROM`，装置切入 LDROM Bootloader（PID `0x3F00`）。
2. `HID_ISP_TOOL.exe firmware.bin` — 装置此时以 `0x3F00` 列举，烧录 APROM 并验证校验和，完成后自动 `RUN_APROM` 返回 CFU 应用。

> 实作细节：传输层 `usb_hid.c` 以 `hid_set_report_ids()` 切换 Report ID，并用 `HidP_GetCaps` 查询各装置的报告长度；
> 命令层 `isp_proto.c` 对两个通道共用同一套封包与序号逻辑。CFU 应用通道的 `GET_FWVER` 会回传完整 4 位元组
> `g_FwVersion`（例 `0x03000002`），而 LDROM Bootloader 回传单一位元组 `0x32`。
