# ISP_HID_20 — m3331 LDROM HSUSBD HID ISP 開機載入器

本範例為 **Nuvoton m3331 (Cortex-M33)** 平台上，透過 **HSUSBD (High-Speed USB Device)** 以 **HID** 介面實作的 **ISP (In-System Programming) 開機載入器 (Bootloader)**。程式駐留於 **LDROM**，開機時依 A/B 雙 Bank 的 Checksum 與韌體版本決定啟動目標；若兩個 Bank 皆無效，則停留於 USB ISP 模式接受主機更新 APROM。

本載入器搭配 [HSUSBD_HID_CFU](../../StdDriver/HSUSBD_HID_CFU/README.md) 範例的 Flash 配置（每 124 KB Bank 末端存放 FW_VERSION 與 FW_CHECKSUM），可作為雙 Bank 韌體升級方案的開機端。

---

## 目錄

1. [專案概覽](#1-專案概覽)
2. [目錄結構](#2-目錄結構)
3. [Flash 分區與開機選擇](#3-flash-分區與開機選擇)
4. [USB HID 設定](#4-usb-hid-設定)
5. [ISP 命令集](#5-isp-命令集)
6. [Keil 專案編譯與燒錄](#6-keil-專案編譯與燒錄)
7. [程式流程](#7-程式流程)
8. [與 HSUSBD_HID_CFU 應用的整合](#8-與-hsusbd_hid_cfu-應用的整合)
9. [注意事項與常見問題](#9-注意事項與常見問題)

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
| 雙 Bank | AP0 (0x00000)、AP1 (0x1F000)，各 124 KB |
| Calib / OEM | Calib (0x3E000)、OEM (0x3F000)，ISP 抹除時保留 |
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

載入器將 256 KB APROM 分為兩個 124 KB 的 AP Bank，末端另保留 Calibration 與 OEM Config 區。每個 Bank 末端 16 bytes 存放版本與 Checksum（與 `gen_checksum_bin.py` 一致）：

```
Bank 末端 16 bytes 配置（相對於各 Bank 起始）
  +0x1EFF0  [4 bytes]  FW_VERSION   韌體版本
  +0x1EFF8  [4 bytes]  FW_CHECKSUM  32-bit byte-sum（slot 計算時視為 0）
```

| 區塊 | 起始位址 | 大小 | 版本 offset | Checksum offset |
|:---:|:---:|:---:|:---:|:---:|
| AP0 | `0x00000` | 124 KB | `0x1EFF0` | `0x1EFF8` |
| AP1 | `0x1F000` | 124 KB | `0x1EFF0` | `0x1EFF8` |
| Calib | `0x3E000` | 4 KB | — | — |
| OEM | `0x3F000` | 4 KB | — | — |

> ISP 更新目標 (`g_u32ApromSize`) 涵蓋 AP0 + AP1 共 248 KB (`0x00000`–`0x3DFFF`)；Calibration (`0x3E000`) 與 OEM Config (`0x3F000`) 位於 APROM 視窗之上，`CMD_UPDATE_APROM` / `CMD_ERASE_ALL` 抹除時會被保留。

### 開機決策邏輯

`main()` 開機時逐一驗證兩個 Bank 的 Checksum（`Boot_ChecksumOK()`），再比較版本：

| AP0 有效 | AP1 有效 | 動作 |
|:---:|:---:|------|
| ✅ | ✅ | 啟動版本較新者（版本相同則優先 AP1） |
| ✅ | ❌ | 啟動 AP0 |
| ❌ | ✅ | 啟動 AP1 |
| ❌ | ❌ | 留在 LDROM，進入 USB ISP 更新模式 |

啟動透過 `Boot_JumpToAP()` → `FMC_SetVectorPageAddr()` 設定向量表並 `NVIC_SystemReset()` 重啟。Checksum 為 `0xFFFFFFFF`（抹除態）視為無效。

> 注意：`main()` 進入上述決策**之前**會先判斷 `RSTSTS & 0x3 == 0`（POR 與 PIN reset 旗標皆清除），若成立則 `goto _LDROM_LOOP` 直接停在 ISP 迴圈。這代表任何「清掉重置旗標後純軟體重置」的路徑都會被困在 LDROM，因此 `CMD_RUN_APROM` 改以向量重映射方式跳轉（見第 5 節）。

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
| `CMD_READ_APROM` | `0xA5` | 讀取 APROM (每次回傳 `READ_APROM_CHUNK` = 48 bytes)，供主機 `--dump` 回讀 |
| `CMD_GET_FWVER` | `0xA6` | 取得 ISP 版本 (0x32) |
| `CMD_RUN_APROM` | `0xAB` | 依開機決策選定 AP Bank，重映射向量頁後重啟至 APROM（見下方註）|
| `CMD_RUN_LDROM` | `0xAC` | 跳轉至 LDROM |
| `CMD_RESET` | `0xAD` | 系統重置 |
| `CMD_CONNECT` | `0xAE` | 建立連線 |
| `CMD_DISCONNECT` | `0xAF` | 中斷連線 |
| `CMD_GET_DEVICEID` | `0xB1` | 取得 Device ID |
| `CMD_UPDATE_DATAFLASH` | `0xC3` | 更新 Data Flash |
| `CMD_WRITE_CHECKSUM` | `0xC9` | 寫入 Checksum |
| `CMD_GET_FLASHMODE` | `0xCA` | 取得 Flash 模式 |
| `CMD_RESEND_PACKET` | `0xFF` | 重送封包 |

> **`CMD_RUN_APROM` 的跳轉方式**：`main()` 在開機時會檢查「POR 與 PIN reset 旗標皆為 0」（`RSTSTS & 0x3 == 0`），若成立便直接 `goto _LDROM_LOOP` 停在 ISP 迴圈、**略過**跳轉 AP 的決策。因此 `CMD_RUN_APROM` 若只是清旗標後做純軟體重置，重啟後會被此短路困在 LDROM。為避免此問題，`CMD_RUN_APROM` 改採與冷開機相同的機制：以 `RunApSelectBank()` 選出 Checksum 有效且版本較新的 Bank，透過 `FMC_SetVectorPageAddr()` 重映射向量頁後 `NVIC_SystemReset()`，令 CPU 直接由 AP 的 reset vector 開機、繞過 LDROM。`CMD_RUN_LDROM` / `CMD_RESET` 則維持清旗標 + BS 位元 + 軟體重置。

---

## 6. Keil 專案編譯與燒錄

1. 以 Keil MDK-ARM 開啟 `Keil/ISP_HID_20.uvprojx`。
2. 編譯後將輸出燒錄至 **LDROM**（須於 CONFIG 設定 `LDROM` 大小並啟用 boot from LDROM）。
3. ISP 操作的目標 Flash 為 AP0 + AP1 (`0x00000`–`0x3DFFF`)，Calibration/OEM 區 (`0x3E000` 以上) 予以保留。
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
         （主機透過 HID 命令更新 APROM，完成後 CMD_RUN_APROM 重映射向量頁跳入 APROM）
```

---

## 8. 與 HSUSBD_HID_CFU 應用的整合

本載入器（LDROM）與 [HSUSBD_HID_CFU](../../StdDriver/HSUSBD_HID_CFU/README.md) 應用（APROM）構成一組雙 Bank 韌體升級方案。兩者共用相同的 Flash 分區、版本/Checksum offset，但以不同 USB 身分列舉：

| 角色 | 程式位置 | USB VID / PID | 更新協議 |
|------|---------|:-------------:|---------|
| CFU 應用 | APROM (AP0/AP1) | `0x0416` / `0xF502` | CFU（升級）＋ HID ISP 通道（讀版本 / 切換開機）|
| 本 ISP 載入器 | LDROM | `0x0416` / `0x3F00` | 傳統 HID ISP（更新 APROM）|

### 8.1 進入 LDROM ISP 的兩種路徑

1. **冷開機 fallback**：開機時 AP0 與 AP1 的 Checksum 皆無效，`main()` 自動停留於 LDROM 進入 USB ISP 模式（見第 3 節）。
2. **由 CFU 應用熱切換**：CFU 應用透過其 HID ISP 通道收到 `CMD_RUN_LDROM (0xAC)` 後，執行**直接向量重映射 + 分支**（不經系統重置）跳入本 LDROM 載入器。跳轉前 CFU 應用會停用 USB/Timer 中斷並強制 USB SE0 斷線，隨後本載入器重新初始化 HSUSBD 並以 PID `0x3F00` 重新列舉。

> CFU 應用的 HID ISP 通道使用 Report ID `40`（Output）/ `41`（Input），封包內容為標準 ISP 格式。該通道僅提供資訊查詢與開機切換（讀版本、DeviceID、FlashMode、RUN_LDROM / RUN_APROM）；**APROM 的實際燒錄** 由本 LDROM 載入器的 `CMD_UPDATE_APROM` 或由 CFU 協議本身完成。

### 8.2 更新後返回 APROM

主機於本載入器完成 APROM 更新並寫入 FW_VERSION / FW_CHECKSUM 後，送出 `CMD_RUN_APROM (0xAB)`。此命令會以 `RunApSelectBank()`（鏡射第 3 節開機決策：Checksum 有效且版本較新的 Bank，平手選 AP1）選定目標，透過 `FMC_SetVectorPageAddr()` 重映射向量頁後 `NVIC_SystemReset()`，令 CPU 直接由該 AP Bank 的 reset vector 開機、回到 CFU 應用。

> 若改送 `CMD_RESET (0xAD)`（清重置旗標 + 軟體重置），重啟後 `main()` 會因 `RSTSTS & 0x3 == 0` 短路而停在 LDROM ISP 迴圈；欲自動返回 APROM 請使用 `CMD_RUN_APROM`。

### 8.3 版本資訊差異

| 命令 | 本 LDROM 載入器 | CFU 應用 ISP 通道 |
|------|----------------|------------------|
| `CMD_GET_FWVER (0xA6)` | 回傳 ISP 載入器版本 `0x32`（單一位元組）| 回傳應用程式 `g_FwVersion`（完整 4 bytes，例 `0x03000002`）|

> 兩者皆走 `CMD_GET_FWVER`，但語意不同：載入器回報自身 Bootloader 版本，CFU 應用回報目前執行的 APROM 韌體版本。主機工具應依連線的 PID（`0x3F00` vs `0xF502`）判讀。

---

## 9. 注意事項與常見問題

### Q1：載入器要燒到哪裡？

燒錄至 **LDROM** 並透過 CONFIG 設定開機自 LDROM 啟動；APROM 留給應用韌體 (AP0/AP1)。

### Q2：開機後一直停在 ISP 模式？

代表 AP0 與 AP1 的 Checksum 皆無效。請確認 APROM 韌體末端的 FW_CHECKSUM (`+0x1EFF8`) 已正確寫入，且為整個 124 KB 影像的 32-bit byte-sum。

### Q3：Checksum/版本 offset 與 CFU 範例一致嗎？

是。FW_VERSION 位於 `+0x1EFF0`、FW_CHECKSUM 位於 `+0x1EFF8`，與 HSUSBD_HID_CFU 的 `gen_checksum_bin.py` 相同（Bank 大小 `0x1F000` = 124 KB）。

### Q4：版本相同時啟動哪個 Bank？

優先啟動 AP1（最近一次更新的 Bank）。

### Q5：從 CFU 應用送出 `CMD_RUN_LDROM` 後找不到裝置？

CFU 應用以直接分支（不經系統重置）跳入本 LDROM 載入器，跳轉後由本載入器重新初始化 HSUSBD 並以 PID `0x3F00` 重新列舉。主機需重新以 PID `0x3F00` 開啟裝置；若一直看不到，請確認 LDROM 已燒錄本載入器且 CONFIG 允許自 LDROM 啟動。

### Q6：燒錄完成後裝置沒有自動跳到 APROM？

舊版 `CMD_RUN_APROM` 只做純軟體重置；因為重置前已清掉 POR/PIN reset 旗標，重啟後 LDROM `main()` 會因 `RSTSTS & 0x3 == 0` 而 `goto _LDROM_LOOP`，**停在 ISP 迴圈不會跳到 APROM**。現已修正：`CMD_RUN_APROM` 改以 `FMC_SetVectorPageAddr()` 重映射向量頁至選定 AP Bank 後 `NVIC_SystemReset()`，繞過 LDROM 直接開機進 APROM。請重新編譯並燒錄本 LDROM 載入器。

---

## 參考資料

- [HSUSBD_HID_CFU 雙 Bank 韌體升級範例](../../StdDriver/HSUSBD_HID_CFU/README.md)
- [HID_ISP_TOOL 主機端 ISP 燒錄/查詢工具](../../../isp_app/HID_ISP_TOOL/README.md)
- Nuvoton m3331 Technical Reference Manual
- Nuvoton BSP: `m3331bsp-master/Library/StdDriver/`
