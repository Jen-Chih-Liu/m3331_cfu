# ISP_HID_20 — m3331 LDROM HSUSBD HID ISP 開機載入器

本範例為 **Nuvoton m3331 (Cortex-M33)** 平台上，透過 **HSUSBD (High-Speed USB Device)** 以 **HID** 介面實作的 **ISP (In-System Programming) 開機載入器 (Bootloader)**。程式駐留於 **LDROM**，開機時依 A/B 雙 Bank 的 Checksum 與韌體版本決定啟動目標；若兩個 Bank 皆無效，則停留於 USB ISP 模式接受主機更新 APROM。

本載入器搭配 [HSUSBD_HID_CFU](../../StdDriver/HSUSBD_HID_CFU/README.md) 範例的 Flash 配置（每 128 KB Bank 末端存放 FW_VERSION 與 FW_CHECKSUM），可作為雙 Bank 韌體升級方案的開機端。

---

## 目錄

1. [專案概覽](#1-專案概覽)
2. [目錄結構](#2-目錄結構)
3. [Flash 分區與開機選擇](#3-flash-分區與開機選擇)
4. [USB HID 設定](#4-usb-hid-設定)
5. [ISP 命令集](#5-isp-命令集)
6. [Keil 專案編譯與燒錄](#6-keil-專案編譯與燒錄)
7. [程式流程](#7-程式流程)
8. [注意事項與常見問題](#8-注意事項與常見問題)

---

## 1. 專案概覽

| 項目 | 說明 |
|------|------|
| 目標晶片 | Nuvoton m3331 (Cortex-M33) |
| 程式位置 | LDROM (USB ISP Bootloader) |
| USB 控制器 | HSUSBD (High-Speed USB Device) |
| 傳輸介面 | HID（64 bytes 封包） |
| USB VID / PID | `0x0416` / `0x3F00` |
| ISP 韌體版本 | `0x32` (FW_VERSION) |
| 系統時脈 | 180 MHz (PLL = HIRC/4)，PCLK = HCLK/2 |
| 雙 Bank | AP0 (0x00000)、AP1 (0x20000)，各 128 KB |
| Debug UART | UART0，115200 bps，PB12/PB13（程式內預設停用）|

---

## 2. 目錄結構

```
ISP_HID_20/
├── main.c                  # 系統初始化、開機選擇、USB ISP 主迴圈
├── descriptors.c           # USB 裝置、組態、HID 描述符
├── hid_transfer.c / .h     # HSUSBD HID 端點設定與中斷處理
├── hsusbd_user.c           # HSUSBD 控制器低階操作
├── isp_user.c / .h         # ISP 命令解析 (ParseCmd) 與命令集定義
├── fmc_user.c / .h         # FMC Flash 讀寫/抹除封裝
├── targetdev.c / .h        # 目標晶片參數 (APROM/DataFlash 大小、DetectPin)
├── startup_m3331_user.S     # LDROM 啟動碼
└── Keil/
    ├── ISP_HID_20.uvprojx  # Keil uVision 專案檔
    └── obj/                # 編譯輸出目錄
```

---

## 3. Flash 分區與開機選擇

載入器將 256 KB APROM 視為兩個 128 KB 的 AP Bank，每個 Bank 末端 16 bytes 存放版本與 Checksum（與 `gen_checksum_bin.py` 一致）：

```
Bank 末端 16 bytes 配置（相對於各 Bank 起始）
  +0x1FFF0  [4 bytes]  FW_VERSION   韌體版本
  +0x1FFF8  [4 bytes]  FW_CHECKSUM  32-bit byte-sum（slot 計算時視為 0）
```

| Bank | 起始位址 | 大小 | 版本 offset | Checksum offset |
|:---:|:---:|:---:|:---:|:---:|
| AP0 | `0x00000` | 128 KB | `0x1FFF0` | `0x1FFF8` |
| AP1 | `0x20000` | 128 KB | `0x1FFF0` | `0x1FFF8` |

### 開機決策邏輯

`main()` 開機時逐一驗證兩個 Bank 的 Checksum（`Boot_ChecksumOK()`），再比較版本：

| AP0 有效 | AP1 有效 | 動作 |
|:---:|:---:|------|
| ✅ | ✅ | 啟動版本較新者（版本相同則優先 AP1） |
| ✅ | ❌ | 啟動 AP0 |
| ❌ | ✅ | 啟動 AP1 |
| ❌ | ❌ | 留在 LDROM，進入 USB ISP 更新模式 |

啟動透過 `Boot_JumpToAP()` → `FMC_SetVectorPageAddr()` 設定向量表並 `NVIC_SystemReset()` 重啟。Checksum 為 `0xFFFFFFFF`（抹除態）視為無效。

---

## 4. USB HID 設定

| 常數 | 值 | 說明 |
|------|----|------|
| `USBD_VID` | `0x0416` | Nuvoton Vendor ID |
| `USBD_PID` | `0x3F00` | ISP HID Product ID |
| 端點封包大小 | 64 bytes | CEP / EPA / EPB 皆 64 |
| INT IN EP | `0x01` | 裝置→主機回應 |
| INT OUT EP | `0x02` | 主機→裝置命令 |

主機每次以 64-byte HID Report 送出命令，裝置以 `ParseCmd()` 解析並由 `g_au8ResponseBuff` 回應。

---

## 5. ISP 命令集

命令定義於 `isp_user.h`，主迴圈呼叫 `ParseCmd()` 處理：

| 命令 | 值 | 說明 |
|------|----|------|
| `CMD_UPDATE_APROM` | `0xA0` | 更新 APROM |
| `CMD_UPDATE_CONFIG` | `0xA1` | 更新 CONFIG |
| `CMD_READ_CONFIG` | `0xA2` | 讀取 CONFIG |
| `CMD_ERASE_ALL` | `0xA3` | 抹除全部 APROM |
| `CMD_SYNC_PACKNO` | `0xA4` | 同步封包序號 |
| `CMD_GET_FWVER` | `0xA6` | 取得 ISP 版本 (0x32) |
| `CMD_RUN_APROM` | `0xAB` | 跳轉至 APROM 執行 |
| `CMD_RUN_LDROM` | `0xAC` | 跳轉至 LDROM |
| `CMD_RESET` | `0xAD` | 系統重置 |
| `CMD_CONNECT` | `0xAE` | 建立連線 |
| `CMD_DISCONNECT` | `0xAF` | 中斷連線 |
| `CMD_GET_DEVICEID` | `0xB1` | 取得 Device ID |
| `CMD_UPDATE_DATAFLASH` | `0xC3` | 更新 Data Flash |
| `CMD_WRITE_CHECKSUM` | `0xC9` | 寫入 Checksum |
| `CMD_GET_FLASHMODE` | `0xCA` | 取得 Flash 模式 |
| `CMD_RESEND_PACKET` | `0xFF` | 重送封包 |

---

## 6. Keil 專案編譯與燒錄

1. 以 Keil MDK-ARM 開啟 `Keil/ISP_HID_20.uvprojx`。
2. 編譯後將輸出燒錄至 **LDROM**（須於 CONFIG 設定 `LDROM` 大小並啟用 boot from LDROM）。
3. APROM 範圍 (`IROM 0–0x7FFFF`) 為 ISP 操作的目標 Flash。
4. 開機後若無有效 APROM，裝置以 VID `0x0416` / PID `0x3F00` 列舉為 HID，可用 Nuvoton ISP 工具或 CFU 主機工具進行更新。

> 預設 Debug UART (PB12/PB13) 在 `SYS_Init()` 中以 `#if 0` 停用，需要除錯輸出時可開啟並取消 `UART_Open()` 註解。

---

## 7. 程式流程

```
開機 (LDROM)
   │
   ├─ SYS_Init(): PLL 180 MHz、啟用 HSUSBD PHY
   ├─ 啟用 ISP (APUEN / DFUEN)
   ├─ Boot_ChecksumOK(AP0) / Boot_ChecksumOK(AP1)
   │     ├─ 皆有效 → 跳轉版本較新 Bank
   │     ├─ 單一有效 → 跳轉該 Bank
   │     └─ 皆無效 ↓
   │
   └─ HSUSBD_Open → HID_Init → 進入 ISP 主迴圈
         while(1): USBD20_IRQHandler → ParseCmd → EPA_Handler
         （主機透過 HID 命令更新 APROM，完成後 CMD_RUN_APROM 啟動）
```

---

## 8. 注意事項與常見問題

### Q1：載入器要燒到哪裡？

燒錄至 **LDROM** 並透過 CONFIG 設定開機自 LDROM 啟動；APROM 留給應用韌體 (AP0/AP1)。

### Q2：開機後一直停在 ISP 模式？

代表 AP0 與 AP1 的 Checksum 皆無效。請確認 APROM 韌體末端的 FW_CHECKSUM (`+0x1FFF8`) 已正確寫入，且為整個 128 KB 影像的 32-bit byte-sum。

### Q3：Checksum/版本 offset 與 CFU 範例一致嗎？

是。FW_VERSION 位於 `+0x1FFF0`、FW_CHECKSUM 位於 `+0x1FFF8`，與 HSUSBD_HID_CFU 的 `gen_checksum_bin.py` 相同。

### Q4：版本相同時啟動哪個 Bank？

優先啟動 AP1（最近一次更新的 Bank）。

---

## 參考資料

- [HSUSBD_HID_CFU 雙 Bank 韌體升級範例](../../StdDriver/HSUSBD_HID_CFU/README.md)
- Nuvoton m3331 Technical Reference Manual
- Nuvoton BSP: `m3331bsp-master/Library/StdDriver/`
