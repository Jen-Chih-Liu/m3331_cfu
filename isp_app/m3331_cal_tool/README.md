# m3331_cal_tool

用於在 Nuvoton M3331 **執行中的 APROM 應用韌體（HSUSBD_HID_CFU）** 下，透過 USB HID
介面對 **Calibration** 與 **OEM config** 資料區進行 erase / write / read 的 PC 端工具。

由於寫入/抹除範圍被韌體嚴格限制在 `0x3E000 ~ 0x3FFFF`，不會動到正在執行的兩個
firmware bank，因此可在應用程式運行時安全更新校正資料與 OEM 設定。

---

## 1. Flash 配置

| 區域    | 位址範圍          | 大小   | 本工具可寫 |
|---------|-------------------|--------|-----------|
| Bank0   | `0x00000–0x1EFFF` | 124 KB | 否        |
| Bank1   | `0x1F000–0x3DFFF` | 124 KB | 否        |
| Calib   | `0x3E000–0x3EFFF` | 4 KB   | **是**    |
| OEM     | `0x3F000–0x3FFFF` | 4 KB   | **是**    |

- Flash page（抹除最小單位）= 1 KB（`0x400`）。
- 可寫區域守護：`FLASH_DATA_BASE = 0x3E000`、`FLASH_DATA_LIMIT = 0x40000`。
- 任何落在此範圍外的 erase / write 會被裝置拒絕並回傳 `status = 1 (RANGE)`。

---

## 2. USB / HID 傳輸層

| 項目            | 值                                    |
|-----------------|---------------------------------------|
| VID / PID       | `0x0416 / 0xF502`（CFU 應用韌體）      |
| 介面            | USB HID（Windows HID class driver）   |
| OUT Report ID   | `40`（host → device）                 |
| IN  Report ID   | `41`（device → host）                 |
| Payload 大小    | 64 bytes（ISP 封包，不含 Report ID）  |

ISP 封包承載在專屬的 HID Report ID 內，與 CFU 韌體更新通道共存於同一介面。
Windows 的 report buffer 會在最前面多一個 Report ID byte，故 OS 層緩衝區比
64-byte payload 多 1 byte。

---

## 3. 封包格式（64 bytes）

### 3.1 Request（Host → Device）

| Bytes   | 欄位            | 說明                                    |
|---------|-----------------|-----------------------------------------|
| `0..3`  | command         | 指令碼（little-endian）                 |
| `4..7`  | packet number   | 封包序號                                |
| `8..11` | address         | flash 位址（erase/write/read）          |
| `12..15`| length          | 位元組長度                              |
| `16..`  | data            | 寫入資料（僅 WRITE_DATA 使用）          |

### 3.2 Response（Device → Host）

| Bytes   | 欄位            | 說明                                    |
|---------|-----------------|-----------------------------------------|
| `0..1`  | checksum        | 整個 request 封包的 16-bit 位元組和     |
| `4..7`  | packet number   | 裝置回覆的序號（= request 序號 + 1）    |
| `8`     | status          | erase / write 的結果碼                  |
| `8..`   | data            | read 回傳的資料位元組                   |

**封包序號握手**：host 每送一筆指令使用序號 `N`，裝置回覆序號 `N+1`；host 收到
相符的回覆後把序號加 2，再送下一筆。

### 3.3 指令碼

| 指令             | 值     | 方向說明                                      |
|------------------|--------|-----------------------------------------------|
| `CMD_CONNECT`    | `0xAE` | 重置裝置封包計數器、建立 session              |
| `CMD_SYNC_PACKNO`| `0xA4` | 同步封包序號                                  |
| `CMD_GET_DEVICEID`| `0xB1`| 讀取 `SYS->PDID`（回傳於 `resp[8..11]`）      |
| `CMD_ERASE_DATA` | `0xD0` | 抹除涵蓋 `[addr, addr+len)` 的所有 page       |
| `CMD_WRITE_DATA` | `0xD1` | 寫入 `len` 位元組（≤ 40，8-byte 對齊）        |
| `CMD_READ_DATA`  | `0xD2` | 讀取 `len` 位元組（≤ 48，4-byte 對齊）        |

### 3.4 Status 結果碼（erase / write）

| 值 | 名稱         | 意義                                      |
|----|--------------|-------------------------------------------|
| 0  | `STS_OK`     | 成功                                      |
| 1  | `STS_RANGE`  | 位址/長度超出 `0x3E000~0x3FFFF` 守護範圍  |
| 2  | `STS_ALIGN`  | 位址/長度非 8-byte 對齊（寫入），或超過每封包上限 |
| 3  | `STS_ERR`    | FMC 抹除/燒錄失敗                         |

### 3.5 每封包上限

- WRITE：每封包最多 **40 bytes** 資料（填於 `req[16..55]`）。
- READ ：每封包最多 **48 bytes** 資料（回於 `resp[8..55]`）。
- 寫入需 8-byte 對齊（m3331 flash 以 64-bit 燒錄），讀取需 4-byte 對齊；工具會自動把 4 KB 區域切成多個封包傳送。

---

## 4. HID API 如何下指令

傳輸層封裝於 [`src/usb_hid.c`](src/usb_hid.c) / [`src/usb_hid.h`](src/usb_hid.h)，
使用 Windows 原生 HID class driver（SetupAPI + hid.dll），**無需** libusb / WinUSB /
Zadig 更換驅動。

典型呼叫流程（參見 [`src/cal_proto.c`](src/cal_proto.c)）：

```c
#include "usb_hid.h"
#include "cal_proto.h"

/* 1. 設定 Report ID 並開啟 CFU 裝置 */
hid_set_report_ids(CAL_REPORT_ID_OUT, CAL_REPORT_ID_IN);   /* 40 / 41 */
hid_open(CAL_USB_VID, CAL_USB_PID);                        /* 0x0416 / 0xF502 */

/* 2. 建立 session */
cal_reset_packno();
cal_connect();        /* CMD_CONNECT   */
cal_sync_packno();    /* CMD_SYNC_PACKNO */

/* 3. 寫入 Calibration（抹除 → 寫入 → 讀回驗證在 main.c 中完成） */
cal_erase(CALIB_BASE, REGION_SIZE);              /* CMD_ERASE_DATA */
cal_write(CALIB_BASE, data, len, progress_cb);   /* CMD_WRITE_DATA（自動分段） */
cal_read (CALIB_BASE, buf,  len, progress_cb);   /* CMD_READ_DATA （自動分段） */

/* 4. 關閉 */
hid_close();
```

底層單封包收送 API：

```c
/* 送出 64-byte payload；傳輸層會自動在前面補 Report ID (40)。 */
int hid_send(const uint8_t *buf, int timeout_ms);

/* 收取一筆 input report；成功回傳 64，逾時回傳 0，錯誤回傳 -1。
 * Report ID (41) 會被自動剝除，只回傳 64 bytes payload。 */
int hid_recv(uint8_t *buf, int timeout_ms);
```

封包組裝（位址、長度、資料欄位的擺放）由 `cmd_packet()` 在
[`src/cal_proto.c`](src/cal_proto.c) 內處理，呼叫者只需使用上述 `cal_*` 函式。

---

## 5. 編譯

需求：CMake（≥ 3.15）與 MSVC 工具鏈（Visual Studio 或 Build Tools）。

```powershell
# 於本資料夾 (isp_app\m3331_cal_tool) 下
cmake -S . -B build -A x64
cmake --build build --config Release
```

產物：`build\Release\m3331_cal_tool.exe`

> 也可由專案根目錄的 `build_all.bat` 一次建置 ISP + CFU 韌體與本工具，
> 產物會收集到 `out\` 目錄。

---

## 6. 使用方式（Shell 指令）

裝置需為執行中的 CFU 應用韌體（`VID 0x0416 / PID 0xF502`）。

```powershell
# 顯示裝置資訊（不存取 flash）
.\build\Release\m3331_cal_tool.exe --info

# 寫入 Calibration 區（抹除 + 燒錄 + 讀回驗證，0x3E000, 4 KB）
.\build\Release\m3331_cal_tool.exe --write-calib calib.bin

# 寫入 OEM config 區（0x3F000, 4 KB）
.\build\Release\m3331_cal_tool.exe --write-oem oem.bin

# 讀出 Calibration 區到檔案（4 KB）
.\build\Release\m3331_cal_tool.exe --read-calib calib_readback.bin

# 讀出 OEM config 區到檔案（4 KB）
.\build\Release\m3331_cal_tool.exe --read-oem oem_readback.bin

# 顯示說明
.\build\Release\m3331_cal_tool.exe --help
```

### 寫入後比對驗證範例

```powershell
# 寫入後再讀回，使用 fc 比對是否一致
.\build\Release\m3331_cal_tool.exe --write-calib calib.bin
.\build\Release\m3331_cal_tool.exe --read-calib calib_readback.bin
fc /b calib.bin calib_readback.bin
```

> 說明：
> - 寫入檔案若小於 4 KB，工具會自動以 `0xFF`（抹除值）補齊到 8-byte 邊界再燒錄。
> - `--write-*` 內建讀回驗證；若讀回內容與寫入資料不符，會回報 `VERIFY FAILED`。
> - 寫入範圍外（非 `0x3E000~0x3FFFF`）時，裝置回傳 `RANGE` 錯誤，工具顯示
>   `device rejected request`。

---

## 7. 原始碼結構

| 檔案                 | 說明                                            |
|----------------------|-------------------------------------------------|
| `src/main.c`         | CLI 參數解析與各操作流程（write/read/info）     |
| `src/cal_proto.c/.h` | ISP 指令層：connect / sync / erase / write / read |
| `src/usb_hid.c/.h`   | Windows 原生 HID 傳輸層（SetupAPI + hid.dll）   |
| `CMakeLists.txt`     | CMake 建置設定（連結 `hid`、`setupapi`）        |

對應的裝置端實作位於
`SampleCode/StdDriver/HSUSBD_HID_CFU/isp_cmd.c`（`CMD_ERASE_DATA` /
`CMD_WRITE_DATA` / `CMD_READ_DATA` 分支）。
