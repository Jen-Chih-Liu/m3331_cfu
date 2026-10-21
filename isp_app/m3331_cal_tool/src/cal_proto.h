/******************************************************************************
 * @file     cal_proto.h
 * @brief    Host-side command layer for the M3331 Calibration / OEM data-flash
 *           access channel (CFU application ISP report IDs 40/41).
 *
 * Protocol summary (64-byte interrupt packets, carried in HID report 40/41):
 *   Request : [0..3]=command, [4..7]=packet number,
 *             [8..11]=address, [12..15]=length, [16..]=data (write only)
 *   Response: [0..1]=16-bit sum of the request packet,
 *             [4..7]=packet number + 1,
 *             [8]   =status (erase/write) or [8..]=data bytes (read)
 * The host advances its packet number by 2 after every acknowledged command.
 ******************************************************************************/
#ifndef CAL_PROTO_H
#define CAL_PROTO_H

#include <stdint.h>

/* USB identifiers: the CFU application exposes the data-flash channel. */
#define CAL_USB_VID        0x0416
#define CAL_USB_PID        0xF502

/* HID report IDs used by the CFU application's ISP channel. */
#define CAL_REPORT_ID_OUT  40
#define CAL_REPORT_ID_IN   41

/* ISP command opcodes (mirror of firmware isp_cmd.h). */
#define CMD_CONNECT        0x000000AE
#define CMD_SYNC_PACKNO    0x000000A4
#define CMD_GET_DEVICEID   0x000000B1
#define CMD_ERASE_DATA     0x000000D0
#define CMD_WRITE_DATA     0x000000D1
#define CMD_READ_DATA      0x000000D2

/* Guarded data region (must match firmware isp_cmd.h). */
#define FLASH_DATA_BASE    0x0003E000UL
#define FLASH_DATA_LIMIT   0x00040000UL
#define CALIB_BASE         0x0003E000UL   /* Calibration data (4 KB) */
#define OEM_BASE           0x0003F000UL   /* OEM config       (4 KB) */
#define REGION_SIZE        0x00001000UL   /* 4 KB per region         */

/* Per-packet payload caps (must match firmware). Writes use FMC 64-bit
 * programming, so the write chunk must stay 8-byte aligned. */
#define CAL_WRITE_CHUNK    40
#define CAL_READ_CHUNK     48

/* Device status codes returned at response byte 8 (mirror firmware). */
#define CAL_STS_OK         0
#define CAL_STS_RANGE      1
#define CAL_STS_ALIGN      2
#define CAL_STS_ERR        3

/* Return codes. */
#define CAL_OK        0
#define CAL_ERR_IO    (-1)  /* USB transfer failure                    */
#define CAL_ERR_TMOUT (-2)  /* No matching ACK within the timeout      */
#define CAL_ERR_STS   (-3)  /* Device returned a non-zero status byte  */
#define CAL_ERR_ARG   (-4)  /* Invalid argument on the host side       */

/** Progress callback (0..100 percent). */
typedef void (*cal_progress_cb)(int percent);

/** Reset the host packet-number counter to its initial value (1). */
void cal_reset_packno(void);

/** CMD_CONNECT: reset the device packet counter and establish a session. */
int cal_connect(void);

/** CMD_SYNC_PACKNO: synchronize the device packet counter with the host. */
int cal_sync_packno(void);

/** CMD_GET_DEVICEID: read SYS->PDID into *pdid. */
int cal_get_deviceid(uint32_t *pdid);

/**
 * CMD_ERASE_DATA: erase every flash page overlapping [addr, addr + len).
 * Only the Calibration / OEM region (0x3E000..0x3FFFF) is accepted.
 */
int cal_erase(uint32_t addr, uint32_t len);

/**
 * CMD_WRITE_DATA: program `len` bytes from `data` to `addr`. `len` must be a
 * multiple of 4; the region must have been erased first. Splits into
 * CAL_WRITE_CHUNK-byte packets.
 */
int cal_write(uint32_t addr, const uint8_t *data, uint32_t len,
              cal_progress_cb progress);

/**
 * CMD_READ_DATA: read `len` bytes starting at `addr` into `data`. `len` must
 * be a multiple of 4. Splits into CAL_READ_CHUNK-byte packets.
 */
int cal_read(uint32_t addr, uint8_t *data, uint32_t len,
             cal_progress_cb progress);

#endif /* CAL_PROTO_H */
