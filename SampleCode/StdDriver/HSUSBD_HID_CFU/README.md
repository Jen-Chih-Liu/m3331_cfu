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
13. [注意事項與常見問題](#13-注意事項與常見問題)

---

## 1. 專案概覽

| 項目 | 說明 |
|------|------|
| 目標晶片 | Nuvoton m3331 (Cortex-M33) |
| USB 控制器 | HSUSBD (High-Speed USB Device) |
| CFU 協議版本 | Revision 2 |
| 升級方式 | A/B 雙 Bank 輪替 (Dual-Bank Swap) |
| USB VID / PID | `0x0416` / `0xF502` |
| 系統時脈 | 180 MHz (HCLK)，PCLK = 90 MHz |
| Debug UART | UART0，115200 bps，PB12 (RXD) / PB13 (TXD) |
| CFU 計時器 | TIMER0 (HIRC 12 MHz)，逾時 5 分鐘 |

---

## 2. 目錄結構

```
HSUSBD_HID_CFU/
├── main.c                          # 系統初始化、USB 開啟、CFU 引擎啟動
├── hid_cfu.h                       # HID Report ID、Usage Page 定義
├── hid_transfer.c / .h             # HSUSBD HID 端點設定與中斷處理
├── descriptors.c                   # USB 裝置、組態、HID 描述符
├── gen_offer_bin.py                # Offer 封包產生工具 (Python)
├── gen_checksum_bin.py             # Checksum 嵌入工具 (Python)
├── gen_content_bin.py              # Content payload 封裝工具 (Python)
├── Keil/
│   ├── HSUSBD_HID_CFU.uvprojx     # Keil uVision 專案檔
│   ├── AP0_OFFSET.sct              # AP0 Scatter 連結腳本 (ROM Base 0x00000)
│   ├── AP1_OFFSET.sct              # AP1 Scatter 連結腳本 (ROM Base 0x20000)
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

本專案採用 **A/B 雙 Bank** 設計，兩個 128 KB 的 AP 分區輪流作為執行區與更新目的區。

```
Flash Address Map (總計 256 KB AP 區域)
┌─────────────────────────────────┐  0x00000
│         AP0  (128 KB)           │  componentId = 0x30
│   執行 or 更新目的地            │
├─────────────────────────────────┤  0x20000
│         AP1  (128 KB)           │  componentId = 0x31
│   執行 or 更新目的地            │
└─────────────────────────────────┘  0x40000
```

### 運作邏輯

| 當前執行 Bank | `ACTIVE_BANK` 定義值 | CFU 寫入目標 | 重啟後啟動 |
|:---:|:---:|:---:|:---:|
| AP0 (0x00000) | `0` | AP1 (0x20000) | AP1 |
| AP1 (0x20000) | `1` | AP0 (0x00000) | AP0 |

更新完成後透過 `FMC_SetVectorPageAddr()` 切換啟動向量，並呼叫 `NVIC_SystemReset()` 重啟。

### Scatter 連結腳本

| 目標 | Scatter 檔案 | ROM Base | ROM Size |
|------|-------------|----------|----------|
| AP0 | `AP0_OFFSET.sct` | `0x00000000` | `0x00020000` (128 KB) |
| AP1 | `AP1_OFFSET.sct` | `0x00020000` | `0x00020000` (128 KB) |

兩個 Scatter 腳本均將 `.fw_version` section 放置於各 Bank ROM 末尾 16 bytes 起始位置（`ROM_BASE + ROM_SIZE - 0x10 = 0x1FFF0`）。Flash 尾端 16 bytes 配置如下：

```
Flash 尾端 16 bytes 配置（每個 128 KB Bank）
  Offset 0x1FFF0  [4 bytes]  FW_VERSION   (.fw_version section, g_FwVersion)
  Offset 0x1FFF4  [4 bytes]  (unused / 0xFF)
  Offset 0x1FFF8  [4 bytes]  FW_CHECKSUM  ← 由 gen_checksum_bin.py 於 post-build 寫入
  Offset 0x1FFFC  [4 bytes]  (unused / 0xFF)
```

> `gen_offer_bin.py` 讀取版本時應使用 `--version-offset 0x1FFF0`（非 `0x1FFFC`）。

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

---

## 5. 韌體版本格式

版本為 32-bit 無號整數，格式如下：

```
 Bit 31..24   Bit 23..8    Bit 7..0
┌────────────┬────────────┬──────────┐
│  Major (8) │ Minor (16) │ Build(8) │
└────────────┴────────────┴──────────┘
```

例：`0x01000002` = Major 0x01 ，Minor 0x0000，Build 0x02

版本常數定義於 `Module/ComponentFwUpdate.c`：

```c
#define FW_VERSION_VALUE   0x01000002u

const uint32_t g_FwVersion __attribute__((section(".fw_version"), used)) = FW_VERSION_VALUE;
```

> **重要**：`.fw_version` section 會被 Scatter 腳本放置在每個 Bank ROM 末尾 16 bytes 起始處（flash offset `0x1FFF0`）。

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
| **AP0** | `ACTIVE_BANK=0` | `AP0_OFFSET.sct` | 燒錄至 0x00000，CFU 更新寫入 AP1 |
| **AP1** | `ACTIVE_BANK=1` | `AP1_OFFSET.sct` | 燒錄至 0x20000，CFU 更新寫入 AP0 |

> **首次燒錄**請選擇 **AP0** 目標，編譯後透過 Nu-Link 燒錄至 0x00000。

### 6.3 關鍵 Preprocessor 設定

在 Keil `Options for Target` → `C/C++` → `Define` 中確認：

```
ACTIVE_BANK=0    ← AP0 目標
ACTIVE_BANK=1    ← AP1 目標
```

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

# 從編譯完成的 .bin 讀取版本 (g_FwVersion 位於 offset 0x1FFF0 = ROM_SIZE-16)
python gen_offer_bin.py --fw-bin Keil/obj/HSUSBD_HID_CFU.bin --version-offset 0x1FFF0

# 從 .bin 讀取版本但不自動遞增 (post-build offer，版本與新韌體相同)
python gen_offer_bin.py --fw-bin Keil/obj/HSUSBD_HID_CFU.bin --version-offset 0x1FFF0 --no-bump

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

1. 將 `.bin` 補齊（或截斷）至 `ROM_SIZE`（128 KB），不足部分以 `0xFF` 填充。
2. 將 offset `0x1FFF8` 的 4-byte Checksum slot 清零。
3. 計算整個映像所有位元組的 **32-bit 加總** (`sum(all bytes) & 0xFFFFFFFF`)。
4. 以 little-endian uint32 形式將結果寫回 offset `0x1FFF8`。
5. 輸出新檔案 `<stem>_sum.bin`。

### 8.2 常用指令

```bash
# 預設：ROM_SIZE=0x20000，輸出 HSUSBD_HID_CFU_sum.bin
python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin

# 明確指定 ROM 大小與輸出路徑
python gen_checksum_bin.py Keil/obj/HSUSBD_HID_CFU.bin --rom-size 0x20000 --output out_sum.bin

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
       --version-offset 0x1FFF0 --no-bump
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
1. `ICompFwUpdateBspCalcCRC()` — 計算已寫入 Bank 的 Checksum
2. `ICompFwUpdateBspAuthenticateFWImage()` — 驗證韌體合法性（目前直接回傳 pass）

#### 步驟 5：切換啟動 Bank 並重啟

`NotifySuccessImpl()` 執行：

```c
#if ACTIVE_BANK == 0
    FMC_SetVectorPageAddr(0x20000);  // 切換至 AP1
#else
    FMC_SetVectorPageAddr(0x00000);  // 切換至 AP0
#endif
NVIC_SystemReset();
```

重啟後裝置以新韌體運行。

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
| `ICompFwUpdateBspCalcCRC()` | 以 `FMC_GetChkSum()` 計算 128 KB Bank checksum |
| `ICompFwUpdateBspAuthenticateFWImage()` | 韌體驗證（目前直接回傳 pass，可加入簽章驗證）|

### Flash 寫入注意事項

- m3331 HSUSBD 使用 `FMC_Write8Bytes()` 進行雙字組寫入，效率較高
- 若資料長度不為 8 的倍數，最後一個 4-byte word 使用 `FMC_Write()`
- 不足 4 bytes 的零散資料以 `0xFF`（抹除態）補齊，避免汙染 Flash

---

## 13. 注意事項與常見問題

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

---

## 參考資料

- [Microsoft CFU 協議規格](https://github.com/microsoft/CFU)
- [CFU-1.0.0 CFU Driver & Protocol 文件](../../../../../../CFU-1.0.0/Documentation/)
- Nuvoton m3331 Technical Reference Manual
- Nuvoton BSP: `m3331bsp-master/Library/StdDriver/`
