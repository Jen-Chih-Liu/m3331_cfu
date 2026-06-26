/******************************************************************************
 * @file     descriptors.c
 * @brief    m3331 HSUSBD CFU HID descriptor file
 *
 * @note
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @copyright Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include "NuMicro.h"
#include "hid_transfer.h"
#include "hid_cfu.h"

/*!< CFU Virtual HID Report Descriptor */
static uint8_t g_CfuVirtualHid_HidReportDescriptor[] =
{
    0x06, CFU_DEVICE_USAGE_PAGE,        // USAGE_PAGE(0xFA00)
    0x09, CFU_DEVICE_USAGE,             // USAGE(0xF5)
    0xA1, 0x01,                         // COLLECTION(0x01)

    0x15, 0x00,                         // LOGICAL_MINIMUM(0)
    0x27, 0xFF, 0xFF, 0xFF, 0xFF,       // LOGICAL_MAXIMUM(-1)

    0x85, REPORT_ID_PAYLOAD_INPUT,      // REPORT_ID(34)
    0x75, INPUT_REPORT_LENGTH,          // REPORT SIZE(32)
    0x95, 0x04,                         // REPORT COUNT(4)
    0x19, PAYLOAD_INPUT_USAGE_MIN,      // USAGE MIN (0x26)
    0x29, PAYLOAD_INPUT_USAGE_MAX,      // USAGE MAX (0x29)
    0x81, 0x02,                         // INPUT(0x02)

    0x85, REPORT_ID_OFFER_INPUT,        // REPORT_ID(37)
    0x75, INPUT_REPORT_LENGTH,          // REPORT SIZE(32)
    0x95, 0x04,                         // REPORT COUNT(4)
    0x19, OFFER_INPUT_USAGE_MIN,        // USAGE MIN (0x1A)
    0x29, OFFER_INPUT_USAGE_MAX,        // USAGE MAX (0x1D)
    0x81, 0x02,                         // INPUT(0x02)

    0x85, REPORT_ID_PAYLOAD_OUTPUT,     // REPORT_ID(33)
    0x75, 0x08,                         // REPORT SIZE(8)
    0x95, OUTPUT_REPORT_LENGTH,         // REPORT COUNT(60)
    0x09, PAYLOAD_OUTPUT_USAGE,         // USAGE(0x31)
    0x92, 0x02, 0x01,                   // OUTPUT(0x02)

    0x85, REPORT_ID_OFFER_OUTPUT,       // REPORT_ID(36)
    0x75, INPUT_REPORT_LENGTH,          // REPORT SIZE(32)
    0x95, 0x04,                         // REPORT COUNT(4)
    0x19, OFFER_OUTPUT_USAGE_MIN,       // USAGE MIN (0x1E)
    0x29, OFFER_OUTPUT_USAGE_MAX,       // USAGE MAX (0x21)
    0x91, 0x02,                         // OUTPUT(0x02)

    0x85, REPORT_ID_VERSIONS_FEATURE,   // REPORT_ID(32)
    0x75, 0x08,                         // REPORT SIZE(8)
    0x95, FEATURE_REPORT_LENGTH,        // REPORT COUNT(60)
    0x09, VERSIONS_FEATURE_USAGE,       // USAGE(0x42)
    0xB2, 0x02, 0x01,                   // FEATURE(0x02)

    0xC0                                // END_COLLECTION()
};

/*----------------------------------------------------------------------------*/
/*!<USB Device Descriptor */
uint8_t gu8DeviceDescriptor[] =
{
    LEN_DEVICE,     /* bLength */
    DESC_DEVICE,    /* bDescriptorType */
    0x00, 0x02,     /* bcdUSB 2.0 */
    0x00,           /* bDeviceClass */
    0x00,           /* bDeviceSubClass */
    0x00,           /* bDeviceProtocol */
    CEP_MAX_PKT_SIZE,   /* bMaxPacketSize0 */
    /* idVendor */
    USBD_CFU_VID & 0x00FF,
    (USBD_CFU_VID & 0xFF00) >> 8,
    /* idProduct */
    USBD_CFU_PID & 0x00FF,
    (USBD_CFU_PID & 0xFF00) >> 8,
    0x00, 0x00,     /* bcdDevice */
    0x01,           /* iManufacture */
    0x02,           /* iProduct */
    0x00,           /* iSerialNumber */
    0x01            /* bNumConfigurations */
};

/*!<USB Qualifier Descriptor */
uint8_t gu8QualifierDescriptor[] =
{
    LEN_QUALIFIER,  /* bLength */
    DESC_QUALIFIER, /* bDescriptorType */
    0x00, 0x02,     /* bcdUSB */
    0x00,           /* bDeviceClass */
    0x00,           /* bDeviceSubClass */
    0x00,           /* bDeviceProtocol */
    CEP_OTHER_MAX_PKT_SIZE, /* bMaxPacketSize0 */
    0x01,           /* bNumConfigurations */
    0x00
};

#define CFG_DESC_TOTAL_LEN  (LEN_CONFIG + LEN_INTERFACE + LEN_HID + LEN_ENDPOINT * 2)

/*!<USB Configure Descriptor (High Speed) */
uint8_t gu8ConfigDescriptor[] =
{
    LEN_CONFIG,     /* bLength */
    DESC_CONFIG,    /* bDescriptorType */
    CFG_DESC_TOTAL_LEN & 0x00FF,
    (CFG_DESC_TOTAL_LEN & 0xFF00) >> 8,
    0x01,           /* bNumInterfaces */
    0x01,           /* bConfigurationValue */
    0x00,           /* iConfiguration */
    0x80 | (USBD_SELF_POWERED << 6) | (USBD_REMOTE_WAKEUP << 5),
    USBD_MAX_POWER,

    /* I/F descr: HID */
    LEN_INTERFACE,
    DESC_INTERFACE,
    0x00,           /* bInterfaceNumber */
    0x00,           /* bAlternateSetting */
    0x02,           /* bNumEndpoints */
    0x03,           /* bInterfaceClass: HID */
    0x00,           /* bInterfaceSubClass */
    0x00,           /* bInterfaceProtocol */
    0x00,           /* iInterface */

    /* HID Descriptor */
    LEN_HID,
    DESC_HID,
    0x10, 0x01,     /* HID Class Spec 1.10 */
    0x00,           /* H/W target country */
    0x01,           /* Number of HID class descriptors */
    DESC_HID_RPT,
    sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0x00FF,
    (sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0xFF00) >> 8,

    /* EP Descriptor: interrupt in (EPA) */
    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_IN_EP_NUM | EP_INPUT),
    EP_INT,
    EPA_MAX_PKT_SIZE & 0x00FF,
    (EPA_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,

    /* EP Descriptor: interrupt out (EPB) */
    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_OUT_EP_NUM | EP_OUTPUT),
    EP_INT,
    EPB_MAX_PKT_SIZE & 0x00FF,
    (EPB_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,
};

/*!<USB Other Speed Configure Descriptor (High Speed -> Full Speed) */
uint8_t gu8OtherConfigDescriptorHS[] =
{
    LEN_CONFIG,
    DESC_OTHERSPEED,
    CFG_DESC_TOTAL_LEN & 0x00FF,
    (CFG_DESC_TOTAL_LEN & 0xFF00) >> 8,
    0x01,
    0x01,
    0x00,
    0x80 | (USBD_SELF_POWERED << 6) | (USBD_REMOTE_WAKEUP << 5),
    USBD_MAX_POWER,

    LEN_INTERFACE,
    DESC_INTERFACE,
    0x00,
    0x00,
    0x02,
    0x03,
    0x00,
    0x00,
    0x00,

    LEN_HID,
    DESC_HID,
    0x10, 0x01,
    0x00,
    0x01,
    DESC_HID_RPT,
    sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0x00FF,
    (sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0xFF00) >> 8,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_IN_EP_NUM | EP_INPUT),
    EP_INT,
    EPA_OTHER_MAX_PKT_SIZE & 0x00FF,
    (EPA_OTHER_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_OUT_EP_NUM | EP_OUTPUT),
    EP_INT,
    EPB_OTHER_MAX_PKT_SIZE & 0x00FF,
    (EPB_OTHER_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,
};

/*!<USB Configure Descriptor (Full Speed) */
uint8_t gu8ConfigDescriptorFS[] =
{
    LEN_CONFIG,
    DESC_CONFIG,
    CFG_DESC_TOTAL_LEN & 0x00FF,
    (CFG_DESC_TOTAL_LEN & 0xFF00) >> 8,
    0x01,
    0x01,
    0x00,
    0x80 | (USBD_SELF_POWERED << 6) | (USBD_REMOTE_WAKEUP << 5),
    USBD_MAX_POWER,

    LEN_INTERFACE,
    DESC_INTERFACE,
    0x00,
    0x00,
    0x02,
    0x03,
    0x00,
    0x00,
    0x00,

    LEN_HID,
    DESC_HID,
    0x10, 0x01,
    0x00,
    0x01,
    DESC_HID_RPT,
    sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0x00FF,
    (sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0xFF00) >> 8,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_IN_EP_NUM | EP_INPUT),
    EP_INT,
    EPA_OTHER_MAX_PKT_SIZE & 0x00FF,
    (EPA_OTHER_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_OUT_EP_NUM | EP_OUTPUT),
    EP_INT,
    EPB_OTHER_MAX_PKT_SIZE & 0x00FF,
    (EPB_OTHER_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,
};

/*!<USB Other Speed Configure Descriptor (Full Speed -> High Speed) */
uint8_t gu8OtherConfigDescriptorFS[] =
{
    LEN_CONFIG,
    DESC_OTHERSPEED,
    CFG_DESC_TOTAL_LEN & 0x00FF,
    (CFG_DESC_TOTAL_LEN & 0xFF00) >> 8,
    0x01,
    0x01,
    0x00,
    0x80 | (USBD_SELF_POWERED << 6) | (USBD_REMOTE_WAKEUP << 5),
    USBD_MAX_POWER,

    LEN_INTERFACE,
    DESC_INTERFACE,
    0x00,
    0x00,
    0x02,
    0x03,
    0x00,
    0x00,
    0x00,

    LEN_HID,
    DESC_HID,
    0x10, 0x01,
    0x00,
    0x01,
    DESC_HID_RPT,
    sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0x00FF,
    (sizeof(g_CfuVirtualHid_HidReportDescriptor) & 0xFF00) >> 8,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_IN_EP_NUM | EP_INPUT),
    EP_INT,
    EPA_MAX_PKT_SIZE & 0x00FF,
    (EPA_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,

    LEN_ENDPOINT,
    DESC_ENDPOINT,
    (CFU_INT_OUT_EP_NUM | EP_OUTPUT),
    EP_INT,
    EPB_MAX_PKT_SIZE & 0x00FF,
    (EPB_MAX_PKT_SIZE & 0xFF00) >> 8,
    HID_DEFAULT_INT_IN_INTERVAL,
};

/*!<USB Language String Descriptor */
uint8_t gu8StringLang[4] =
{
    4,
    DESC_STRING,
    0x09, 0x04
};

/*!<USB Vendor String Descriptor */
uint8_t gu8VendorStringDesc[] =
{
    16,
    DESC_STRING,
    'N', 0, 'u', 0, 'v', 0, 'o', 0, 't', 0, 'o', 0, 'n', 0
};

/*!<USB Product String Descriptor */
uint8_t gu8ProductStringDesc[] =
{
    22,
    DESC_STRING,
    'H', 0, 'I', 0, 'D', 0, ' ', 0, 'C', 0, 'F', 0, 'U', 0,
    ' ', 0, 'D', 0, 'e', 0
};

uint8_t *gpu8UsbString[4] =
{
    gu8StringLang,
    gu8VendorStringDesc,
    gu8ProductStringDesc,
    NULL
};

uint8_t *gu8UsbHidReport[3] =
{
    g_CfuVirtualHid_HidReportDescriptor,
    NULL,
    NULL
};

uint32_t gu32UsbHidReportLen[3] =
{
    sizeof(g_CfuVirtualHid_HidReportDescriptor),
    0,
    0
};

uint32_t gu32ConfigHidDescIdx[3] =
{
    (LEN_CONFIG + LEN_INTERFACE),
    0,
    0
};

S_HSUSBD_INFO_T gsHSInfo =
{
    gu8DeviceDescriptor,
    gu8ConfigDescriptor,
    gpu8UsbString,
    gu8QualifierDescriptor,
    gu8ConfigDescriptorFS,
    gu8OtherConfigDescriptorHS,
    gu8OtherConfigDescriptorFS,
    NULL,
    gu8UsbHidReport,
    gu32UsbHidReportLen,
    gu32ConfigHidDescIdx
};
