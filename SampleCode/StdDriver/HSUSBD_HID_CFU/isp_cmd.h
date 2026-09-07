/******************************************************************************
 * @file     isp_cmd.h
 * @brief    USB HID ISP command layer for the HSUSBD CFU sample.
 *
 * A trimmed port of the ISP_HID_20 bootloader protocol so the CFU application
 * can answer version/info/handshake queries and hand control to the LDROM ISP
 * bootloader.  The 64-byte ISP packet is carried inside a dedicated HID
 * Report ID (see hid_cfu.h), leaving a 63-byte payload.
 *
 *   Request  : [0..3]=command, [4..7]=packet number / parameter
 *   Response : [0..1]=16-bit sum of the request payload,
 *              [4..7]=packet number, [8..]=response data
 *
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#ifndef __ISP_CMD_H__
#define __ISP_CMD_H__

#include <stdint.h>

/* ISP command opcodes (subset shared with isp_user.h). */
#define CMD_SYNC_PACKNO     0x000000A4
#define CMD_GET_FWVER       0x000000A6
#define CMD_RUN_APROM       0x000000AB
#define CMD_RUN_LDROM       0x000000AC
#define CMD_CONNECT         0x000000AE
#define CMD_DISCONNECT      0x000000AF
#define CMD_GET_DEVICEID    0x000000B1
#define CMD_GET_FLASHMODE   0x000000CA

/* Deferred boot requests handled by the main loop (never inside the ISR). */
extern volatile uint8_t g_u8JumpLdromPending;
extern volatile uint8_t g_u8JumpApromPending;

/**
 * Parse one ISP request payload and build the response payload.
 *
 * @param pu8Buffer   Request payload (command at offset 0).
 * @param u8len       Number of valid request bytes (used for the checksum).
 * @param pu8Response Response payload buffer (>= 63 bytes).
 * @return 0 on success.
 */
int ParseISPCmd(uint8_t *pu8Buffer, uint8_t u8len, uint8_t *pu8Response);

#endif  /* __ISP_CMD_H__ */
