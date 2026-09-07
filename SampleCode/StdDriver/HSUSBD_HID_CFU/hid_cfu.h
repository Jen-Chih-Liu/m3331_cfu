/******************************************************************************
 * @file     hid_cfu.h
 * @brief    m3331 CFU series HSUSBD driver header file
 *
 * @note
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#ifndef __USBD_HID_CFU_H__
#define __USBD_HID_CFU_H__

/* Define the vendor id and product id */
#define USBD_CFU_VID        0x0416
#define USBD_CFU_PID        0xF502

#define CFU_DEVICE_USAGE_PAGE       0x00, 0xFA
#define CFU_DEVICE_USAGE            0xF5
#define REPORT_ID_DUMMY_INPUT       32
#define DUMMY_INPUT_USAGE           0x52
#define REPORT_ID_PAYLOAD_INPUT     34
#define INPUT_REPORT_LENGTH         32
#define PAYLOAD_INPUT_USAGE_MIN     0x26
#define PAYLOAD_INPUT_USAGE_MAX     0x29
#define REPORT_ID_OFFER_INPUT       37
#define OFFER_INPUT_USAGE_MIN       0x1A
#define OFFER_INPUT_USAGE_MAX       0x1D
#define REPORT_ID_PAYLOAD_OUTPUT    33
#define OUTPUT_REPORT_LENGTH        60
#define PAYLOAD_OUTPUT_USAGE        0x31
#define REPORT_ID_OFFER_OUTPUT      36
#define OFFER_OUTPUT_USAGE_MIN      0x1E
#define OFFER_OUTPUT_USAGE_MAX      0x21
#define REPORT_ID_VERSIONS_FEATURE  32
#define FEATURE_REPORT_LENGTH       60
#define VERSIONS_FEATURE_USAGE      0x42

/*---------------------------------------------------------------------------*/
/* USB HID ISP protocol channel (coexists with CFU on the same interface)    */
/*   Carries the Nuvoton 64-byte ISP packet inside a dedicated Report ID.     */
/*   Report ID consumes byte 0, leaving 63 payload bytes per packet.          */
/*---------------------------------------------------------------------------*/
#define REPORT_ID_ISP_OUTPUT        40      /* host -> device (interrupt OUT) */
#define REPORT_ID_ISP_INPUT         41      /* device -> host (interrupt IN)  */
#define ISP_REPORT_LENGTH           63      /* payload bytes after Report ID  */
#define ISP_OUTPUT_USAGE            0x60
#define ISP_INPUT_USAGE             0x61

#endif  /* __USBD_HID_CFU_H__ */
