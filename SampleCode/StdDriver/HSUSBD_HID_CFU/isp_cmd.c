/******************************************************************************
 * @file     isp_cmd.c
 * @brief    USB HID ISP command handler for the HSUSBD CFU sample.
 *
 * Implements the subset of the Nuvoton HID ISP protocol needed to report
 * device/version information and to hand control over to the LDROM ISP
 * bootloader.  Flash programming stays in the CFU engine, so no erase/write
 * commands are implemented here.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include "NuMicro.h"
#include "isp_cmd.h"

/* Application firmware version, defined in ComponentFwUpdate.c */
extern const uint32_t g_FwVersion;

volatile uint8_t g_u8JumpLdromPending = 0;
volatile uint8_t g_u8JumpApromPending = 0;

/* FMC boot-select (BS) bit inside FMC->ISPCTL: 0 = APROM, 1 = LDROM. */
#define ISP_ISPCTL_BS_Msk   0x2u

static uint16_t ISP_Checksum(uint8_t *buf, int len)
{
    int i;
    uint16_t c;

    for (c = 0, i = 0; i < len; i++)
        c += buf[i];

    return c;
}

int ParseISPCmd(uint8_t *pu8Buffer, uint8_t u8len, uint8_t *pu8Response)
{
    static uint32_t u32PackNo = 1;
    uint32_t u32Lcmd;
    uint16_t u16Lcksum;
    uint8_t *pu8Src = pu8Buffer;

    u32Lcmd = inpw((uint32_t)pu8Src);
    outpw((uint32_t)(pu8Response + 4), 0);

    /* Populate CONFIG0/CONFIG1 at [8..15] for host-side compatibility. */
    outpw((uint32_t)(pu8Response + 8),  FMC_Read(FMC_CONFIG_BASE));
    outpw((uint32_t)(pu8Response + 12), FMC_Read(FMC_CONFIG_BASE + 4));

    if (u32Lcmd == CMD_SYNC_PACKNO)
    {
        u32PackNo = inpw((uint32_t)(pu8Src + 4));
    }
    else if (u32Lcmd == CMD_GET_FWVER)
    {
        /* Return the full 32-bit application version. */
        outpw((uint32_t)(pu8Response + 8), g_FwVersion);
    }
    else if (u32Lcmd == CMD_GET_DEVICEID)
    {
        outpw((uint32_t)(pu8Response + 8), SYS->PDID);
    }
    else if (u32Lcmd == CMD_GET_FLASHMODE)
    {
        /* 1 = booting from APROM, 2 = booting from LDROM. */
        outpw((uint32_t)(pu8Response + 8),
              (FMC->ISPCTL & ISP_ISPCTL_BS_Msk) ? 2 : 1);
    }
    else if (u32Lcmd == CMD_CONNECT)
    {
        u32PackNo = 1;
    }
    else if (u32Lcmd == CMD_RUN_LDROM)
    {
        /* Deferred to the main loop so the ack reaches the host first. */
        g_u8JumpLdromPending = 1;
    }
    else if (u32Lcmd == CMD_RUN_APROM)
    {
        g_u8JumpApromPending = 1;
    }
    /* CMD_DISCONNECT and unknown commands: acknowledge with no side effect. */

    u16Lcksum = ISP_Checksum(pu8Buffer, u8len);
    outps((uint32_t)pu8Response, u16Lcksum);
    ++u32PackNo;
    outpw((uint32_t)(pu8Response + 4), u32PackNo);
    u32PackNo++;

    return 0;
}
