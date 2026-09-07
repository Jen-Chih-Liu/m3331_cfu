# HSUSBD_HID_CFU — m3331 Component Firmware Update (CFU) 範例

本範例示範如何在 **Nuvoton m3331 (Cortex-M33)** 平台上，透過 **HSUSBD (High-Speed USB Device)** 介面以 **HID** 通訊實作 **Microsoft Component Firmware Update (CFU)** 韌體升級協議。

---

## 目錄

1. [專案概覽](#1-專案概覽)
2. [目錄結構](#2-目錄結構)
3. [Flash 分區配置](#3-flash-分區配置)
4. [USB HID 描述符設定](#4-usb-hid-描述符設定)
5. [韌體版本格式](#5-韌體版本格式)
6. [Keil 專案編譯設定](#6-keil-專案編譯設定)
7. [Offer 封包產生工具 (gen_offer_bin.py)](#7-offer-封包產生工具-gen_offer_binpy)
8. [Checksum 嵌入工具 (gen_checksum_bin.py)](#8-checksum-嵌入工具-gen_checksum_binpy)
9. [Content 封包產生工具 (gen_content_bin.py)](#9-content-封包產生工具-gen_content_binpy)
10. [Post-Build 自動化 (post_build.bat)](#10-post-build-自動化-post_buildbat)
11. [CFU 升級流程](#11-cfu-升級流程)
12. [BSP 移植層說明](#12-bsp-移植層說明)
13. [USB HID ISP 協議通道](#13-usb-hid-isp-協議通道)
14. [注意事項與常見問題](#14-注意事項與常見問題)

---

## 1. 專案概覽

| 項目 | 說明 |
|------|------|
| 目標晶片 | Nuvoton m3331 (Cortex-M33) |
| USB 控制器 | HSUSBD (High-Speed USB Device) |
| CFU 協議版本 | Revision 2 |
| 升級方式 | A/B 雙 Bank 輪替 (Dual-Bank Swap) |
| 附加協議 | USB HID ISP 通道（讀版本資訊、跳轉 LDROM，與 CFU 共用同一介面）|
| USB VID / PID | `0x0416` / `0xF502` |
| 系統時脈 | 180 MHz (HCLK)，PCLK = 90 MHz |
| Debug UART | UART0，115200 bps，PB12 (RXD) / PB13 (TXD) |
| CFU 計時器 | TIMER0 (HIRC 12 MHz)，逾時 5 分鐘 |

---

## 2. 目錄結構

```
HSUSBD_HID_CFU/
├── main.c                          # 系統初始化、USB 開啟、CFU 引擎啟動、LDROM 跳轉
├── hid_cfu.h                       # HID Report ID、Usage Page、ISP 通道定義
├── hid_transfer.c / .h             # HSUSBD HID 端點設定與中斷處理、ISP/CFU 路由
├── isp_cmd.c / .h                  # USB HID ISP 命令解析 (讀版本、跳轉 LDROM/APROM)
├── descriptors.c                   # USB 裝置、組態、HID 描述符
├── gen_offer_bin.py                # Offer 封包產生工具 (Python)
├── gen_checksum_bin.py             # Checksum 嵌入工具 (Python)
├── gen_content_bin.py              # Content payload 封裝工具 (Python)
├── Keil/
│   ├── HSUSBD_HID_CFU.uvprojx     # Keil uVision 專案檔
│   ├── AP0_OFFSET.sct              # AP0 Scatter 連結腳本 (ROM Base 0x00000)
│   ├── AP1_OFFSET.sct              # AP1 Scatter 連結腳本 (ROM Base 0x1F000)
│   ├── post_build.bat              # 編譯後自動產生 checksum/offer/content
│   └── obj/                        # 編譯輸出目錄
└── Module/
    ├── ComponentFwUpdate.c / .h    # CFU 協議核心實作
    ├── ICompFwUpdateBsp.c / .h     # BSP 移植層 (Flash 操作)
    ├── IComponentFirmwareUpdate.h  # CFU 介面定義
    ├── coretypes.h                 # 基本型別定義
    ├── FwVersion.h                 # 韌體版本位元欄位定義
    └── McuStatus.h                 # MCU 狀態碼定義
```

---

## 3. Flash 分區配置

本專案採用 **A/B 雙 Bank** 設計，兩個 124 KB 的 AP 分區輪流作為執行區與更新目的區，並保留 Calibration 與 OEM config 區。

```
Flash Address Map (總計 256 KB)
┌─────────────────────────────────┐  0x00000
│         AP0  (124 KB)           │  componentId = 0x30
│   執行 or 更新目的地            │
├─────────────────────────────────┤  0x1F000
│         AP1  (124 KB)           │  componentId = 0x31
│   執行 or 更新目的地            │
├─────────────────────────────────┤  0x3E000
│    Calibration data (4 KB)      │
├─────────────────────────────────┤  0x3F000
│      OEM config     (4 KB)      │
└─────────────────────────────────┘  0x40000
```

### 運作邏輯

| 當前執行 Bank | `ACTIVE_BANK` 定義值 | CFU 寫入目標 | 重啟後啟動 |
|:---:|:---:|:---:|:---:|
| AP0 (0x00000) | `0` | AP1 (0x1F000) | AP1 |
| AP1 (0x1F000) | `1` | AP0 (0x00000) | AP0 |

更新完成後透過 `FMC_SetVectorPageAddr()` 切換啟動向量，並呼叫 `NVIC_SystemReset()` 重啟。

### Scatter 連結腳本

| 目標 | Scatter 檔案 | ROM Base | ROM Size |
|------|-------------|----------|----------|
| AP0 | `AP0_OFFSET.sct` | `0x00000000` | `0x0001F000` (124 KB) |
| AP1 | `AP1_OFFSET.sct` | `0x0001F000` | `0x0001F000` (124 KB) |

兩個 Scatter 腳本均將 `.fw_version` section 放置於各 Bank ROM 末尾 16 bytes 起始位置（`ROM_BASE + ROM_SIZE - 0x10 = 0x1EFF0`）。Flash 尾端 16 bytes 配置如下：

```
Flash 尾端 16 bytes 配置（每個 124 KB Bank）
  Offset 0x1EFF0  [4 bytes]  FW_VERSION   (.fw_version section, g_FwVersion)
  Offset 0x1EFF4  [4 bytes]  (unused / 0xFF)
  Offset 0x1EFF8  [4 bytes]  FW_CHECKSUM  ← 由 gen_checksum_bin.py 於 post-build 寫入
  Offset 0x1EFFC  [4 bytes]  (unused / 0xFF)
```

> `gen_offer_bin.py` 讀取版本時應使用 `--version-offset 0x1EFF0`（非 `0x1EFFC`）。

---

## 4. USB HID 描述符設定

HID Report 使用專有 Usage Page，定義於 `hid_cfu.h`：

| 常數 | 值 | 說明 |
|------|----|------|
| `USBD_CFU_VID` | `0x0416` | Nuvoton Vendor ID |
| `USBD_CFU_PID` | `0xF502` | Product ID |
| `CFU_DEVICE_USAGE_PAGE` | `0xFA00` | CFU 專用 Usage Page |
| `CFU_DEVICE_USAGE` | `0xF5` | CFU Device Usage |

### HID Report ID 對照表

| Report ID | 方向 | 長度 | 用途 |
|-----------|------|------|------|
| `32` (0x20) | Feature | 60 bytes | **Versions Feature** — 回報所有 Component 版本 |
| `33` (0x21) | Output  | 60 bytes | **Payload Output** — 主機送出韌體資料封包 |
| `34` (0x22) | Input   | 32 bytes | **Payload Input** — 裝置回應寫入結果 |
| `36` (0x24) | Output  | — | **Offer Output** — 主機送出版本 Offer |
| `37` (0x25) | Input   | — | **Offer Input** — 裝置回應 Accept / Reject |
| `40` (0x28) | Output  | 63 bytes | **ISP Output** — 主機送出 ISP 命令封包（見第 13 節）|
| `41` (0x29) | Input   | 63 bytes | **ISP Input** — 裝置回應 ISP 命令結果 |

> Report ID `40` / `41` 為附加的 **USB HID ISP 通道**，與 CFU 共用同一 HID 介面。`EPB_Handler()` 依封包首位元組（Report ID）將資料路由至 ISP 或 CFU 處理常式。

---

## 5. 韌體版本格式

版本為 32-bit 無號整數，格式如下：

```
 Bit 31..24   Bit 23..8    Bit 7..0
┌────────────┬────────────┬──────────┐
│  Major (8) │ Minor (16) │ Build(8) │
└────────────┴────────────┴──────────┘
```

例：`0x03000002` = Major 0x03 ，Minor 0x0000，Build 0x02

版本常數定義於 `Module/ComponentFwUpdate.c`：

```c
#define FW_VERSION_VALUE   0x03000002u

const uint32_t g_FwVersion __attribute__((section(".fw_version"), used)) = FW_VERSION_VALUE;
```

> **重要**：`.fw_version` section 會被 Scatter 腳本放置在每個 Bank ROM 末尾 16 bytes 起始處（flash offset `0x1EFF0`）。

### 版本比較規則

`ProcessOfferImpl()` 使用簡單的整數比較：

```c
if (pCommand->version > g_FwVersion)
    pResponse->status = FIRMWARE_UPDATE_OFFER_ACCEPT;   // 接受升級
else
    pResponse->status = FIRMWARE_UPDATE_OFFER_REJECT;   // 拒絕 (版本相同或更舊)
```

---

## 6. Keil 專案編譯設定

### 6.1 開啟專案

以 Keil MDK-ARM 開啟：`Keil/HSUSBD_HID_CFU.uvprojx`

### 6.2 選擇編譯目標

專案包含兩個編譯目標，對應 A/B 雙 Bank：

| Target 名稱 | Preprocessor Define | Scatter 檔 | 說明 |
|-------------|---------------------|-----------|------|
| **`HSUSBD_HID_CFU_AP0`** | `ACTIVE_BANK=0` | `AP0_OFFSET.sct` | 燒錄至 0x00000，CFU 更新寫入 AP1 |
| **`HSUSBD_HID_CFU_AP1`** | `ACTIVE_BANK=1` | `AP1_OFFSET.sct` | 燒錄至 0x1F000，CFU 更新寫入 AP0 |

> **首次燒錄**請選擇 **`HSUSBD_HID_CFU_AP0`** 目標，編譯後透過 Nu-Link 燒錄至 0x00000。

### 6.3 關鍵 Preprocessor 設定

在 Keil `Options for Target` → `C/C++` → `Define` 中確認各目標的預處理器定義：

```
ACTIVE_BANK=0    ← HSUSBD_HID_CFU_AP0 目標
ACTIVE_BANK=1    ← HSUSBD_HID_CFU_AP1 目標
```

> [!WARNING]
> **注意**：在 Keil 專案檔中，請特別確認 `HSUSBD_HID_CFU_AP1` 目標的 Define 必須包含 `ACTIVE_BANK=1`。若誤設為 0，AP1 運行時會將自身視為 AP0，導致 CFU 升級時覆寫目前正在執行的 Bank 1。

### 6.4 編譯輸出

編譯成功後輸出於 `Keil/obj/`：

- `HSUSBD_HID_CFU.axf` — ELF 執行檔
- `HSUSBD_HID_CFU.bin` — 純二進位 (用於 gen_offer_bin.py)
- `HSUSBD_HID_CFU.hex` — Intel HEX (用於燒錄)

---

## 7. Offer 封包產生工具 (gen_offer_bin.py)

`gen_offer_bin.py` 為 Python 3 腳本，用於產生 CFU 所需的 16-byte **offer.bin** 檔案。

### 7.1 前置需求

- Python 3.6 以上
- 無額外套件需求

### 7.2 常用指令

```bash
# 自動從原始碼偵測版本並自動遞增，產生 offer.bin
python gen_offer_bin.py

# 指定版本號
python gen_offer_bin.py --version 0x02000000

# 從編譯完成的 .bin 讀取版本 (g_FwVersion 位於 offset 0x1EFF0 = ROM_SIZE-16)
python gen_offer_bin.py --fw-bin Keil/obj/HSUSBD_HID_CFU.bin --version-offset 0x1EFF0

# 從 .bin 讀取版本但不自動遞增 (post-build offer，版本與新韌體相同)
python gen_offer_bin.py --fw-bin Keil/obj/HSUSBD_HID_CFU.bin --version-offset 0x1EFF0 --no-bump

# 指定 Component ID (AP0=0x30, AP1=0x31)
python gen_offer_bin.py --component 0x31 --output VirtualDevice_AP1.offer.bin

# 強制忽略版本檢查 (forceIgnoreVersion bit)
python gen_offer_bin.py --force-version

# 強制更新後立即重啟
python gen_offer_bin.py --force-reset

# 僅顯示當前版本資訊，不產生檔案
python gen_offer_bin.py --info
```

### 7.3 版本來源優先順序

1. `--version 0xXXXXXXXX` （最高優先，強制指定）
2. `--fw-bin <file> --version-offset <offset>` （從 .bin 讀取已編譯版本，預設自動 +1 major；加 `--no-bump` 使用原版本）
3. 自動從 `Module/ComponentFwUpdate.c` 偵測 `FW_VERSION_VALUE` 並 +1 major

### 7.4 Offer.bin 結構 (16 bytes)

```
Byte  0    : Segment Number
Byte  1    : Reserved / Flags (forceIgnoreVersion, forceReset)
Byte  2    : Component ID (0x30=AP0, 0x31=AP1)
Byte  3    : Token
Byte  4-7  : Firmware Version (uint32 LE，格式見第 5 節)
Byte  8-11 : HW Variant Mask (uint32 LE)
Byte  12   : Protocol Revision (=2) | Bank
Byte  13   : Milestone
Byte  14-15: Product ID (uint16 LE)
```

---

## 8. Checksum 嵌入工具 (gen_checksum_bin.py)

`gen_checksum_bin.py` 為 Python 3 腳本，用於在 post-build 階段將 **32-bit 字節累加 Checksum** 寫入韌體 `.bin` 的固定位置。

### 8.1 Checksum 演算法

1. 將 `.bin` 補齊（或截斷）至 `ROM_SIZE`（124 KB），不足部分以 `0xFF` 填充。
2. 計算 offset `0x00000` 至 `0x1EFF7`（共 `0x1EFF8` 位元組）所有位元組的 **32-bit 加總** (`sum(raw[0:0x1EFF8]) & 0xFFFFFFFF`)。Checksum 槽（`0x1EFF8..0x1EFFB`）及尾端剩餘 padding（`0x1EFFC..0x1EFFF`）不計入加總，此範圍與 LDROM `Boot_ChecksumOK()` 完全一致。
3. 以 little-endian uint32 形式將加總結果寫入 offset `0x1EFF8`。
4. 輸出新檔案 `<stem>_sum.bin`。

### 8.2 常用指令

```bash
# 預設：ROM_SIZE=0x1F000，輸出 HSUSBD_HID_CFU_sum.bin
python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin

# 明確指定 ROM 大小與輸出路徑
python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin --rom-size 0x1F000 --output out_sum.bin

# 僅顯示 FW_VERSION 與現有 Checksum slot 內容，不產生輸出
python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin --info
```

### 8.3 建議 Post-Build 流程

整體流程已由 `Keil/post_build.bat` 自動化（見第 10 節），手動執行對應步驟如下：

```
1. Keil 編譯完成 → 產生 Keil/obj/<name>.bin
2. python gen_checksum_bin.py Keil/obj/<name>.bin
   → 輸出 Keil/obj/<name>_sum.bin（含 Checksum）
3. python gen_offer_bin.py --fw-bin Keil/obj/<name>_sum.bin \
       --version-offset 0x1EFF0 --no-bump
   → 產生 <name>.offer.bin
4. python gen_content_bin.py Keil/obj/<name>_sum.bin --output Keil/obj/<name>_content.bin
   → 將 _sum.bin 重新封裝為 CFU content payload
5. 使用 FwUpdateCfu.exe 進行 CFU 升級：
   FwUpdateCfu.exe update Cfg.cfg <name>.offer.bin <name>_content.bin
```

---

## 9. Content 封包產生工具 (gen_content_bin.py)

`gen_content_bin.py` 將原始韌體 `.bin` 重新封裝為 `FwUpdateCfu.exe`（`ProcessSrecBin`）所需的 **content payload** 格式。注意輸入的 `_sum.bin` 並非主機直接讀取的格式，必須先轉換。

### 9.1 Payload 記錄格式

連續重複以下記錄，直到資料結束：

```
[4 bytes] address  (little-endian)
[1 byte ] length   (本塊資料長度，最大 52)
[length ] data     (不足 4 倍數時補 0xFF)
```

### 9.2 常用指令

```bash
# 預設 chunk=52，封裝為 content.bin
python gen_content_bin.py Keil/obj/HSUSBD_HID_CFU_sum.bin --output content.bin

# post_build.bat 使用 chunk=48，並可指定起始位址
python gen_content_bin.py Keil/obj/HSUSBD_HID_CFU_sum.bin --output content.bin --chunk 48 --base 0x0
```

> `--chunk` 必須為 4 的倍數且 ≤ 52；預設 52，自動化腳本採用 48。

---

## 10. Post-Build 自動化 (post_build.bat)

`Keil/post_build.bat` 由 Keil **AfterMake → UserProg2** 以 `post_build.bat @L` 呼叫（`%1` 為輸出名，如 `HSUSBD_HID_CFU_AP0` / `HSUSBD_HID_CFU_AP1`），依序自動完成四個步驟：

| 步驟 | 工具 | 輸出 |
|:---:|------|------|
| 1 | `fromelf --text -c` | `<name>.txt`（反組譯） |
| 2 | `gen_checksum_bin.py` | `<name>_sum.bin`（Checksum 寫入 ROM_SIZE−8） |
| 3 | `gen_offer_bin.py` | `<name>.offer.bin` |
| 4 | `gen_content_bin.py` | `<name>_content.bin`（chunk 48） |

### Component ID 自動偵測

Offer 的 `componentId` 必須對應 **目標裝置目前執行** 的 Bank：

| 編譯目標 | 安裝至 | 目標裝置正在執行 | Offer componentId |
|:---:|:---:|:---:|:---:|
| AP0 build | AP0 | AP1 | `0x31` |
| AP1 build | AP1 | AP0 | `0x30` |

> 需先設定 `ARMCLANG_BIN`（預設 `C:\Keil_v5\ARM\ARMCLANG\bin`）以使 `fromelf` 可用。

---

## 11. CFU 升級流程

### 11.1 整體流程圖

```
Host (Windows CFU Driver)              Device (m3331)
        │                                     │
        │─── GET_FEATURE (Report 32) ────────►│  取得 Component 版本清單
        │◄─── Versions Response ──────────────│
        │                                     │
        │─── SET_REPORT Offer (Report 36) ───►│  送出 Offer (版本、Component ID)
        │◄─── Offer Response (Report 37) ─────│  Accept / Reject / Busy
        │                                     │
        │  (若 Accept)                         │
        │─── SET_REPORT Payload (Report 33) ─►│  分塊傳輸韌體資料 (60 bytes/封包)
        │◄─── Payload Response (Report 34) ───│  每塊回應 Success / Error
        │  ... 重複直到 Last Block ...         │
        │                                     │
        │─── SET_REPORT Offer END ───────────►│  通知傳輸結束
        │◄─── Offer Response ─────────────────│  SWAP_PENDING
        │                                     │
        │                                     │  → FMC_SetVectorPageAddr() 切換 Boot Bank
        │                                     │  → NVIC_SystemReset() 重啟
        │                                     │  (以新韌體啟動)
```

### 11.2 詳細步驟說明

#### 步驟 1：取得韌體版本

主機透過 `GET_FEATURE` 讀取 Report ID 32，裝置回傳所有已註冊 Component 的版本資訊。

#### 步驟 2：發送 Offer

主機發送 Offer 封包 (Report ID 36)，包含：
- 目標 Component ID (0x30 或 0x31)
- 新韌體版本號
- HW Variant Mask

裝置 `ProcessOfferImpl()` 比較版本：
- **ACCEPT**：新版本 > 當前版本 → 開始接受內容封包
- **REJECT**：版本相同或更舊 → 拒絕
- **BUSY**：有其他更新進行中

#### 步驟 3：傳輸韌體內容

主機以 60 bytes 為單位分塊傳送韌體二進位資料 (Report ID 33)。

每塊包含旗標：
- `FIRMWARE_UPDATE_FLAG_FIRST_BLOCK` (0x80)：第一塊
- `FIRMWARE_UPDATE_FLAG_LAST_BLOCK` (0x40)：最後一塊
- `FIRMWARE_UPDATE_FLAG_VERIFY` (0x08)：要求驗證

裝置 `ICompFwUpdateBspWrite()` 將資料寫入非活躍 Bank。

#### 步驟 4：寫入完成驗證

傳輸完成後，裝置執行：
1. 查詢 CRC 偏移行為：`GetCrcOffsetImpl()` 回傳 `MCU_STATUS_CFU_CRC_CHECK_NOT_REQUIRED`，略過讀回 CRC 比對步驟（`ICompFwUpdateBspCalcCRC()` 函式保留備用）。
2. `ICompFwUpdateBspAuthenticateFWImage()` — 驗證韌體合法性（目前直接回傳 pass）。

#### 步驟 5：切換啟動 Bank 並重啟

`NotifySuccessImpl()` 切換啟動向量並標記重啟：

```c
#if ACTIVE_BANK == 0
    FMC_SetVectorPageAddr(0x1F000);  // 切換至 AP1
#else
    FMC_SetVectorPageAddr(0x00000);  // 切換至 AP0
#endif
    FMC_Close();

    g_u8ResetPending = 1;            // 標記重啟需求，交由 main 迴圈延遲執行
```

`main()` 主迴圈偵測到 `g_u8ResetPending` 後，延遲一段時間讓主機順利讀取最後一個 content SUCCESS 回應封包，隨後呼叫 `NVIC_SystemReset()` 重啟，避免 USB 突然中斷導致主機報錯。重啟後裝置以新韌體運行。

### 11.3 升級逾時保護

若升級在 **5 分鐘** 內未完成，TIMER0 中斷會觸發 `_UpdateTimerCallback()`，強制將 `updateInProgress` 設為 FALSE，中止本次升級。

---

## 12. BSP 移植層說明

`Module/ICompFwUpdateBsp.c` 為平台相關實作，移植時需修改以下函式：

| 函式 | 說明 |
|------|------|
| `ICompFwUpdateBspPrepare()` | 抹除非活躍 Bank 所有 4 KB Page |
| `ICompFwUpdateBspWrite()` | 以 `FMC_Write8Bytes()` 寫入 Flash（每次 8 bytes）|
| `ICompFwUpdateBspRead()` | 以 `FMC_Read()` 讀回已寫入資料 |
| `ICompFwUpdateBspCalcCRC()` | 以 `FMC_GetChkSum()` 計算 124 KB Bank checksum |
| `ICompFwUpdateBspAuthenticateFWImage()` | 韌體驗證（目前直接回傳 pass，可加入簽章驗證）|

### Flash 寫入注意事項

- m3331 HSUSBD 使用 `FMC_Write8Bytes()` 進行雙字組寫入，效率較高
- 若資料長度不為 8 的倍數，最後一個 4-byte word 使用 `FMC_Write()`
- 不足 4 bytes 的零散資料以 `0xFF`（抹除態）補齊，避免汙染 Flash

### Calibration / OEM config 區存取

`ICompFwUpdateBsp.c` 另提供兩個**非韌體資料區**（位於 A/B Bank 之外，CFU 升級不會覆寫）的存取 API：

| 區域 | 位址 | 大小巨集 |
|------|------|---------|
| Calibration data | `0x3E000 – 0x3EFFF` | `CFU_CALIB_REGION_SIZE` (4 KB) |
| OEM config | `0x3F000 – 0x3FFFF` | `CFU_OEM_REGION_SIZE` (4 KB) |

| 函式 | 說明 |
|------|------|
| `ICompFwUpdateBspCalibErase()` / `ICompFwUpdateBspOemErase()` | 抹除整個區域（寫入前必須先抹除）|
| `ICompFwUpdateBspCalibWrite(offset, pData, length)` / `...OemWrite(...)` | 寫入資料，`offset` 相對區域起始，內含邊界檢查 |
| `ICompFwUpdateBspCalibRead(offset, pData, length)` / `...OemRead(...)` | 讀回資料，支援任意長度（含尾端 1–3 bytes）|

使用範例：

```c
uint8_t calib[16];
SYS_UnlockReg();
FMC_Open();
FMC_ENABLE_AP_UPDATE();          /* 允許寫入 APROM */

ICompFwUpdateBspCalibErase();                       /* 先抹除 4 KB 區 */
ICompFwUpdateBspCalibWrite(0, myCalibData, 16);     /* 寫入 */
ICompFwUpdateBspCalibRead(0, calib, 16);            /* 讀回驗證 */

FMC_Close();
SYS_LockReg();
```

> **注意**：Flash 只能將位元由 1 清為 0，重寫前務必先呼叫對應的 `...Erase()`。呼叫前需已 `FMC_Open()` 並 `FMC_ENABLE_AP_UPDATE()`（`main.c` 已於啟動時開啟）。

---

## 13. USB HID ISP 協議通道

除了 CFU 升級協議外，本範例額外整合 **Nuvoton USB HID ISP** 協議通道，讓主機可在不進入 CFU 流程的情況下**讀取版本資訊**並**跳轉至 LDROM ISP Bootloader**。ISP 與 CFU 共用同一個 HID 介面，藉由專屬的 **Report ID** 區分。

### 13.1 通道設計

| 項目 | 值 | 說明 |
|------|----|------|
| ISP Output Report ID | `40` (0x28) | 主機 → 裝置，攜帶 ISP 命令封包 |
| ISP Input Report ID | `41` (0x29) | 裝置 → 主機，回應 ISP 命令結果 |
| Payload 長度 | 63 bytes | 64-byte 封包扣除 Report ID (byte 0) |
| 端點 | EPB (OUT) / EPA (IN) | 與 CFU 共用，皆為 64-byte Interrupt |

`hid_transfer.c` 的 `EPB_Handler()` 依封包首位元組（Report ID）路由：Report ID `40` → `ISP_HandleOutReport()`（`isp_cmd.c`），其餘 → CFU `CFU_GetOutReport()`。ISP 回應以 Report ID `41` 透過 EPA 傳回。

> ISP 封包的 63-byte payload 仍沿用標準 ISP 佈局：`[0..3]=command`、`[4..7]=packet number`、`[8..]=data`。因僅使用資訊/交握類命令，捨棄第 64 byte 無影響。

### 13.2 支援的 ISP 命令

| 命令 | 值 | 功能 | 回應 (payload) |
|------|----|----|---------------|
| `CMD_CONNECT` | `0xAE` | 建立連線，重置封包計數 (PackNo=1) | ack |
| `CMD_SYNC_PACKNO` | `0xA4` | 同步封包計數 | ack |
| `CMD_GET_FWVER` | `0xA6` | **讀取版本資訊** | `[8..11]` = `g_FwVersion`（4 bytes）|
| `CMD_GET_DEVICEID` | `0xB1` | 讀取裝置 ID | `[8..11]` = `SYS->PDID` |
| `CMD_GET_FLASHMODE` | `0xCA` | 查詢開機模式 | `[8]` = 1 (APROM) / 2 (LDROM) |
| `CMD_RUN_LDROM` | `0xAC` | **跳轉至 LDROM ISP Bootloader** | ack（隨後跳轉）|
| `CMD_RUN_APROM` | `0xAB` | 重啟至 APROM | ack（隨後 `NVIC_SystemReset()`）|
| `CMD_DISCONNECT` | `0xAF` | 結束連線 | ack（無副作用）|

> **版本來源**：`CMD_GET_FWVER` 回傳完整 4-byte 應用程式版本 `g_FwVersion`（例 `0x03000002`），格式同第 5 節，而非獨立 ISP Bootloader 的單一位元組 `0x32`。

### 13.3 回應封包格式

```
Byte  0-1  : 請求封包的 16-bit 位元組加總 (checksum)
Byte  2-3  : Reserved
Byte  4-7  : Packet number (裝置遞增)
Byte  8-.. : 回應資料 (版本 / DeviceID / FlashMode 等)
```

### 13.4 跳轉至 LDROM 流程

`CMD_RUN_LDROM` 不會在中斷內立即跳轉，而是設定 `g_u8JumpLdromPending` 旗標，先送出 ISP 回應（讓主機讀到 ACK），隨後交由 `main()` 主迴圈的 `ISP_JumpToLdrom()` 進行斷線與重置：

```c
NVIC_DisableIRQ(USBD20_IRQn);          /* 停用 USB / TIMER 中斷 */
NVIC_DisableIRQ(TMR0_IRQn);
HSUSBD_SET_SE0();                      /* 強制 USB 斷線，令主機重新枚舉 */

SYS_UnlockReg();
outpw(&SYS->RSTSTS, 3);                /* 清除 POR 與 PIN reset 標誌，確保 LDROM 開機停在 ISP 迴圈 */
FMC_Open();
FMC_SetVectorPageAddr(FMC_LDROM_BASE); /* 0x0F100000 重映射至向量頁 */
NVIC_SystemReset();                    /* 系統重置開機進 LDROM */
```

重啟後，LDROM `main()` 偵測到 `(RSTSTS & 0x3) == 0`，略過 APROM Checksum 檢驗並直接進入 USB ISP 主迴圈（PID `0x3F00`），等待主機進行燒錄。

### 13.5 主機端相容性

主機工具 [`isp_app/HID_ISP_TOOL`](../../../isp_app/HID_ISP_TOOL) 已支援本 CFU 應用的 ISP 通道，並會**自動偵測**目標裝置：

- 先嘗試 LDROM ISP Bootloader（PID `0x3F00`，無 Report ID 的 raw 64-byte 封包）。
- 找不到時改連本 CFU 應用（PID `0xF502`），並自動於封包首位元組前置 Report ID **`40`**、讀取回應時解析 Report ID **`41`**。

常用操作：

```powershell
# 讀回裝置資訊；連到 CFU 應用時會印出 4-byte APROM 韌體版本 (g_FwVersion)
HID_ISP_TOOL.exe --info

# 令 CFU 應用切換進 LDROM ISP Bootloader（送出 CMD_RUN_LDROM）
HID_ISP_TOOL.exe --run-ldrom
```

> CFU 應用的 ISP 通道僅提供資訊查詢與開機切換；APROM 的實際燒錄需先 `--run-ldrom` 進入 LDROM Bootloader，再以 `HID_ISP_TOOL.exe firmware.bin` 燒錄。詳見該工具的 README。

---

## 14. 注意事項與常見問題

### Q1：首次燒錄應使用哪個 Target？

**使用 AP0 Target**，燒錄至 Flash 0x00000。之後透過 CFU 升級 AP1，再由 AP1 升級 AP0，如此輪替。

### Q2：如何確認目前運行的是 AP0 還是 AP1？

觀察 UART0 Debug 輸出，或確認當前 `g_FwVersion` 的版本號。  
亦可讀取 `FMC_GetVECMAP()` 確認目前啟動向量位址。

### Q3：offer.bin 應指定 Component ID 0x30 還是 0x31？

- 若要升級「執行 AP0 的裝置」，offer 應指定 `--component 0x30`（AP0 報告的 Component ID）
- 若要升級「執行 AP1 的裝置」，offer 應指定 `--component 0x31`

### Q4：Offer 被 Reject (FIRMWARE_OFFER_REJECT_OLD_FW) 怎麼辦？

新版本號必須 **嚴格大於** 當前韌體版本。使用 `gen_offer_bin.py` 時請確認版本號已遞增，或使用 `--force-version` 強制略過版本比較。

### Q5：升級後 USB 裝置無法被偵測到？

確認 `FMC_SetVectorPageAddr()` 已正確設定，且重啟後 USB 重新枚舉。  
可透過 UART0 觀察 `"NotifySuccessImpl: rebooting..."` 輸出確認流程是否完成。

### Q6：`FMC_Erase` 失敗？

確認 `SYS_UnlockReg()` 和 `FMC_ENABLE_AP_UPDATE()` 已在 main 中呼叫，且抹除位址對齊至 4 KB (`FMC_FLASH_PAGE_SIZE`)。

### Q7：ISP 通道（Report ID 40/41）與 CFU 會互相干擾嗎？

不會。兩者共用同一 HID 介面，`EPB_Handler()` 依 Report ID 分流；ISP 與 CFU 傳輸不會同時進行，回應緩衝區可安全共用。

### Q8：送出 `CMD_RUN_LDROM` 後主機找不到裝置？

`ISP_JumpToLdrom()` 會先清除重置旗標、重映射向量頁至 LDROM 後執行 `NVIC_SystemReset()`。跳轉後由 LDROM ISP Bootloader 重新初始化 USB 並以 PID `0x3F00` 列舉。主機需重新開啟 PID `0x3F00` 裝置；若找不到，請確認 LDROM 已燒錄可運作的 ISP Bootloader 且 CONFIG 允許自 LDROM 啟動。

---

## 參考資料

- [Microsoft CFU 協議規格](https://github.com/microsoft/CFU)
- [Microsoft CFU 開源專案與 Protocol 文件](https://github.com/microsoft/CFU)
- [HID_ISP_TOOL 主機端 ISP 燒錄/查詢工具](../../../isp_app/HID_ISP_TOOL/README.md)
- [ISP_HID_20 LDROM HID ISP 開機載入器](../../ISP/ISP_HID_20/README.md)
- Nuvoton m3331 Technical Reference Manual
- Nuvoton BSP: `m3331bsp-master/Library/StdDriver/`
