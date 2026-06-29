/******************************************************************************
 * @file     hid_transfer.h
 * @brief    m3331 HSUSBD CFU HID transfer header file
 *
 * @note
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#ifndef __USBD_HID_H__
#define __USBD_HID_H__

#include "hid_cfu.h"

/*---------------------------------------------------------------------------*/
/* UART debug message switch                                                  */
/*   Set CFU_DEBUG to 1 to enable UART debug prints, 0 to disable them.       */
/*   Use CFU_DBG(...) in place of printf for debug-only messages.            */
/*---------------------------------------------------------------------------*/
#ifndef CFU_DEBUG
#define CFU_DEBUG   1
#endif

#if CFU_DEBUG
#include <stdio.h>
#define CFU_DBG(...)   printf(__VA_ARGS__)
#else
#define CFU_DBG(...)   ((void)0)
#endif

/* Define the vendor id and product id (CFU uses hid_cfu.h values) */
#define USBD_VID        USBD_CFU_VID
#define USBD_PID        USBD_CFU_PID

/*!<Define HID Class Specific Request */
#define GET_REPORT          0x01
#define GET_IDLE            0x02
#define GET_PROTOCOL        0x03
#define SET_REPORT          0x09
#define SET_IDLE            0x0A
#define SET_PROTOCOL        0x0B

/*!<USB HID Interface Class protocol */
#define HID_NONE            0x00
#define HID_KEYBOARD        0x01
#define HID_MOUSE           0x02

/*!<USB HID Class Report Type */
#define HID_RPT_TYPE_INPUT      0x01
#define HID_RPT_TYPE_OUTPUT     0x02
#define HID_RPT_TYPE_FEATURE    0x03

/*-------------------------------------------------------------*/
/* Define EP maximum packet size
 * CFU uses 64-byte interrupt endpoints for compatibility.
 */
#define CEP_MAX_PKT_SIZE        64
#define CEP_OTHER_MAX_PKT_SIZE  64
#define EPA_MAX_PKT_SIZE        64
#define EPA_OTHER_MAX_PKT_SIZE  64
#define EPB_MAX_PKT_SIZE        64
#define EPB_OTHER_MAX_PKT_SIZE  64

#define CEP_BUF_BASE    0
#define CEP_BUF_LEN     CEP_MAX_PKT_SIZE
#define EPA_BUF_BASE    0x200
#define EPA_BUF_LEN     EPA_MAX_PKT_SIZE
#define EPB_BUF_BASE    0x600
#define EPB_BUF_LEN     EPB_MAX_PKT_SIZE

/* Define the interrupt In/Out EP number */
#define CFU_INT_IN_EP_NUM   0x01
#define CFU_INT_OUT_EP_NUM  0x02

/* Define Descriptor information */
#define HID_DEFAULT_INT_IN_INTERVAL     10
#define USBD_SELF_POWERED               0
#define USBD_REMOTE_WAKEUP              0
#define USBD_MAX_POWER                  50  /* The unit is in 2mA. ex: 50 * 2mA = 100mA */

/*-------------------------------------------------------------*/
extern volatile uint8_t s_Get_FwVer_Resp[60];
extern uint8_t volatile g_u8Suspend;

/* CFU IN buffer for EPA transfers */
extern uint8_t g_u8CfuInBuf[64];
extern volatile uint32_t g_u32CfuInLen;
extern volatile uint8_t g_u8CfuInReady;

/* Set after a successful update + bank switch; main loop reboots into new bank */
extern volatile uint8_t g_u8ResetPending;

/*-------------------------------------------------------------*/
void HID_Init(void);
void HID_InitForHighSpeed(void);
void HID_InitForFullSpeed(void);
void HID_ClassRequest(void);
void HID_VendorRequest(void);

void EPA_Handler(void);
void EPB_Handler(void);
void CFU_SetInReport(void);
void CFU_GetOutReport(uint8_t *pu8EpBuf, uint32_t u32Size);

#endif  /* __USBD_HID_H__ */
