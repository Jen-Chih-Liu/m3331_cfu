/******************************************************************************
 * @file     isp_proto.h
 * @brief    Nuvoton M3331 HID ISP command layer (host side).
 *
 * Protocol summary (64-byte interrupt packets):
 *   Request : [0..3]=command, [4..7]=packet number,
 *             [8..11]=address, [12..15]=length, [16..63]=data
 *   Response: [0..1]=16-bit sum of the request packet,
 *             [4..7]=packet number + 1, [8..]=response data
 * The host advances its packet number by 2 after every acknowledged command.
 ******************************************************************************/
#ifndef ISP_PROTO_H
#define ISP_PROTO_H

#include <stdint.h>

/* USB identifiers. Two firmware targets speak the HID ISP protocol:           *
 *   LDROM ISP bootloader (ISP_HID_20)   : PID 0x3F00, un-numbered reports.     *
 *   CFU application ISP channel (HSUSBD_HID_CFU) : PID 0xF502, report IDs 40/41.*/
#define ISP_USB_VID       0x0416
#define ISP_USB_PID       0x3F00   /* default: LDROM bootloader (raw reports) */
#define ISP_USB_PID_LDROM 0x3F00
#define ISP_USB_PID_CFU   0xF502

/* HID report IDs used by the CFU application's ISP channel. */
#define ISP_REPORT_ID_OUT 40
#define ISP_REPORT_ID_IN  41

/* ISP command opcodes (mirror of firmware isp_user.h / isp_cmd.h). */
#define CMD_UPDATE_APROM     0x000000A0
#define CMD_READ_CONFIG      0x000000A2
#define CMD_READ_APROM       0x000000A5
#define CMD_SYNC_PACKNO      0x000000A4
#define CMD_GET_FWVER        0x000000A6
#define CMD_RUN_APROM        0x000000AB
#define CMD_RUN_LDROM        0x000000AC
#define CMD_CONNECT          0x000000AE
#define CMD_GET_DEVICEID     0x000000B1
#define CMD_GET_FLASHMODE    0x000000CA

/* Bytes of APROM returned per CMD_READ_APROM response (matches firmware). */
#define READ_APROM_CHUNK     48

/* Return codes. */
#define ISP_OK        0
#define ISP_ERR_IO    (-1)  /* USB transfer failure */
#define ISP_ERR_TMOUT (-2)  /* No matching ACK within the timeout */

/** Progress callback for APROM programming (0..100 percent). */
typedef void (*isp_progress_cb)(int percent);

/** Reset the host packet-number counter to its initial value (1). */
void isp_reset_packno(void);

/** CMD_CONNECT: reset the device packet counter and establish a session. */
int isp_connect(void);

/** CMD_SYNC_PACKNO: synchronize the device packet counter with the host. */
int isp_sync_packno(void);

/** CMD_GET_DEVICEID: read SYS->PDID into *pdid. */
int isp_get_deviceid(uint32_t *pdid);

/** CMD_GET_FWVER: read the 1-byte ISP firmware version into *ver. */
int isp_get_fwver(uint8_t *ver);

/**
 * CMD_GET_FWVER (CFU application channel): read the full 4-byte application
 * firmware version (g_FwVersion) into *ver.
 */
int isp_get_fwver32(uint32_t *ver);

/** CMD_READ_CONFIG: read CONFIG0/CONFIG1 into the provided outputs. */
int isp_read_config(uint32_t *cfg0, uint32_t *cfg1);

/** CMD_GET_FLASHMODE: read boot mode (1 = APROM, 2 = LDROM) into *mode. */
int isp_get_flashmode(uint32_t *mode);

/**
 * CMD_UPDATE_APROM: erase and program APROM with `size` bytes from `data`.
 * On success, *out_checksum receives the 16-bit flash checksum reported by
 * the device (compare against isp_calc_checksum() for verification).
 */
int isp_update_aprom(const uint8_t *data, uint32_t size,
                     isp_progress_cb progress, uint16_t *out_checksum);

/**
 * CMD_READ_APROM: read `size` bytes of APROM starting at `addr` into `data`.
 * Issues one read command per READ_APROM_CHUNK bytes and reassembles them.
 * Only supported by the LDROM bootloader (raw report) channel.
 */
int isp_read_aprom(uint32_t addr, uint8_t *data, uint32_t size,
                   isp_progress_cb progress);

/** CMD_RUN_APROM: leave the bootloader and jump to the application. */
int isp_run_aprom(void);

/** CMD_RUN_LDROM: leave the application and jump to the LDROM ISP bootloader. */
int isp_run_ldrom(void);

/** Host-side 16-bit checksum matching the device CalCheckSum() semantics. */
uint16_t isp_calc_checksum(const uint8_t *data, uint32_t size);

#endif /* ISP_PROTO_H */
