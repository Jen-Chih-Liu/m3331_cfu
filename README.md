# m3331_cfu — Nuvoton M3331 雙 Bank 韌體升級方案

本專案針對 **Nuvoton M3331**（Arm Cortex-M33，180 MHz，內建 HSUSBD 高速 USB）實作一套完整的 **USB 韌體升級（Firmware Update）解決方案**，同時支援兩種主機端升級協議，並提供對應的 PC 端工具與一鍵建置腳本。

- **CFU（Component Firmware Update）** — 微軟標準韌體升級協議，跑在 APROM 應用中，支援 AP0 / AP1 雙 Bank 熱更新。
- **HID ISP（In-System Programming）** — Nuvoton 傳統 HID ISP 協議，跑在 LDROM Bootloader，可直接燒錄 / 回讀 APROM。

兩者共用同一套 Flash 分區與版本 / Checksum 配置，並可互相切換開機。

---

## 1. 專案架構

```
m3331_cfu/
├── build_all.bat          # 一鍵建置：ISP + CFU(AP0/AP1) + PC 工具，收集到 out/
├── merge_aprom.bat        # 合併 AP0 + AP1 sum bin → 248 KB 完整 APROM 映像
├── out/                   # 預先建置好的 .bin 與工具（已隨 repo 提供）
│
├── isp_app/
│   └── HID_ISP_TOOL/      # PC 端 HID ISP 燒錄 / 查詢 / 回讀工具（CMake / MSVC）
│
├── SampleCode/
│   ├── ISP/
│   │   └── ISP_HID_20/    # LDROM 端 HID ISP Bootloader（PID 0x3F00）
│   └── StdDriver/
│       └── HSUSBD_HID_CFU/# APROM 端 CFU 應用 + HID ISP 通道（PID 0xF502）
│
└── Library/               # Nuvoton M3331 BSP（CMSIS / Device / StdDriver）
```

各子專案皆附獨立 README，詳細說明見：

| 子專案 | 說明 | 文件 |
|--------|------|------|
| `SampleCode/StdDriver/HSUSBD_HID_CFU` | CFU 雙 Bank 應用（APROM）＋ HID ISP 通道 | [README](SampleCode/StdDriver/HSUSBD_HID_CFU/README.md) |
| `SampleCode/ISP/ISP_HID_20` | LDROM HID ISP Bootloader | [README](SampleCode/ISP/ISP_HID_20/README.md) |
| `isp_app/HID_ISP_TOOL` | PC 端主機工具 | [README](isp_app/HID_ISP_TOOL/README.md) |

---

## 2. Flash 記憶體配置

M3331 APROM 共 256 KB，分為兩個 124 KB 應用 Bank，外加校準與 OEM 設定區：

| 區塊 | 起始位址 | 大小 | 說明 |
|------|:--------:|:----:|------|
| **AP0**（Bank 0） | `0x00000` | 124 KB | 應用韌體 Bank 0 |
| **AP1**（Bank 1） | `0x1F000` | 124 KB | 應用韌體 Bank 1 |
| Calibration | `0x3E000` | 4 KB | 校準資料（升級時保留） |
| OEM Config | `0x3F000` | 4 KB | OEM 設定（升級時保留） |

每個 Bank 末端存放版本與 Checksum：

- `bank_base + 0x1EFF0` [4B] **FW_VERSION**
- `bank_base + 0x1EFF8` [4B] **FW_CHECKSUM**（整個 124 KB 影像的 32-bit byte-sum，Checksum 槽本身以 0 計）

> ISP 更新目標涵蓋 AP0 + AP1 共 248 KB（`0x00000`–`0x3DFFF`）；Calibration / OEM 位於 APROM 視窗之上，抹除時會被保留。

---

## 3. 兩種 USB 身分與升級路徑

| 角色 | 執行位置 | VID / PID | Report ID | 主要功能 |
|------|---------|:---------:|:---------:|---------|
| **CFU 應用** | APROM (AP0/AP1) | `0x0416` / `0xF502` | CFU / ISP `40`·`41` | CFU 熱更新 ＋ HID ISP 資訊查詢 / 開機切換 |
| **ISP Bootloader** | LDROM | `0x0416` / `0x3F00` | 無（原始 64 bytes） | HID ISP 燒錄 / 回讀 APROM |

### 開機決策（LDROM `main()`）

裝置上電後永遠先進 LDROM，驗證兩個 Bank 的 Checksum 並比較版本：

| AP0 有效 | AP1 有效 | 動作 |
|:---:|:---:|------|
| ✅ | ✅ | 啟動版本較新者（版本相同優先 AP1） |
| ✅ | ❌ | 啟動 AP0 |
| ❌ | ✅ | 啟動 AP1 |
| ❌ | ❌ | 留在 LDROM，進入 USB ISP 更新模式 |

啟動透過 `FMC_SetVectorPageAddr()` 重映射向量頁後 `NVIC_SystemReset()`，令 CPU 直接由 AP 的 reset vector 開機。

---

## 4. 快速開始

### 4.1 使用預建好的檔案（out/）

`out/` 目錄已隨 repo 提供以下建置產物，可直接使用：

| 檔案 | 用途 |
|------|------|
| `ISP_HID_20.bin` | 燒錄至 **LDROM** 的 ISP Bootloader |
| `HSUSBD_HID_CFU_AP0*.bin` / `AP1*.bin` | CFU 各 Bank 影像（含 offer / content / sum）|
| `HSUSBD_HID_CFU_APROM_248KB.BIN` | 合併後的完整 248 KB APROM 映像 |
| `HID_ISP_TOOL.exe` | PC 端 HID ISP 工具 |
| `FwUpdateCfu.exe` / `Cfg.cfg` | 微軟 CFU 主機更新工具與設定 |

### 4.2 從原始碼建置

```powershell
# 需先安裝 Keil MDK（UV4）與 CMake + MSVC（建議在 VS Developer Prompt 執行）
.\build_all.bat        # 建置 ISP + CFU(AP0/AP1) + HID_ISP_TOOL，收集到 out\
.\merge_aprom.bat      # 合併 AP0 + AP1 sum bin → out\HSUSBD_HID_CFU_APROM_248KB.BIN
```

> `build_all.bat` 內的 `UV4` 變數預設為 `C:\Keil_v5\UV4\UV4.exe`，如安裝路徑不同請自行修改。

### 4.3 使用 HID_ISP_TOOL 燒錄 / 查詢

工具會自動偵測目標裝置在 LDROM（`0x3F00`）或 CFU 應用（`0xF502`）通道：

```powershell
# 查詢裝置資訊（Device ID / 韌體版本 / CONFIG / 開機模式）
HID_ISP_TOOL.exe --info

# 燒錄 APROM、驗證 Checksum，完成後自動跳回應用（僅 LDROM 通道）
HID_ISP_TOOL.exe HSUSBD_HID_CFU_APROM_248KB.BIN

# 裝置正跑 CFU 應用時，先切換進 LDROM Bootloader，再重跑燒錄
HID_ISP_TOOL.exe --run-ldrom

# 將整顆 256 KB APROM 回讀存成 .bin（僅 LDROM 通道）
HID_ISP_TOOL.exe --dump aprom_backup.bin
```

裝置以標準 HID 列舉，由 Windows HID 類別驅動接管，**無需安裝任何驅動**。

---

## 5. 典型升級流程

```
裝置正跑 CFU 應用 (PID 0xF502)
   │
   ├─ (A) CFU 協議熱更新：FwUpdateCfu.exe 直接升級對側 Bank，重啟切換
   │
   └─ (B) HID ISP 路徑：
         1. HID_ISP_TOOL.exe --run-ldrom     切入 LDROM Bootloader (PID 0x3F00)
         2. HID_ISP_TOOL.exe firmware.bin     燒錄 APROM + 驗證 Checksum
         3. 完成後 CMD_RUN_APROM 重映射向量頁，開機回 CFU 應用
```

---

## 6. 開發環境

| 工具 | 用途 |
|------|------|
| Keil MDK-ARM（UV4） | 建置韌體（ISP_HID_20、HSUSBD_HID_CFU AP0/AP1） |
| CMake + MSVC（x64） | 建置 PC 端 `HID_ISP_TOOL`（連結 `hid` + `setupapi`） |
| Nuvoton M3331 BSP | `Library/`（CMSIS / Device / StdDriver） |

---

## 7. 參考資料

- [HSUSBD_HID_CFU 雙 Bank CFU 應用](SampleCode/StdDriver/HSUSBD_HID_CFU/README.md)
- [ISP_HID_20 LDROM Bootloader](SampleCode/ISP/ISP_HID_20/README.md)
- [HID_ISP_TOOL PC 端工具](isp_app/HID_ISP_TOOL/README.md)
- Nuvoton M3331 Technical Reference Manual
- [Microsoft Component Firmware Update (CFU) 協議](https://github.com/microsoft/CFU)
