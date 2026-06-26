/******************************************************************************
 * @file     ICompFwUpdateBsp.c
 * @brief    m3331 CFU BSP implementation
 *           Ported from M2351 (USBD) to m3331 (HSUSBD).
 *
 * @note
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include "coretypes.h"
#include "NuMicro.h"

/*
 * m3331 has 512 KB flash (0x00000000 - 0x0007FFFF).
 * Bank 0: 0x00000000 - 0x0003FFFF (256 KB) - application firmware
 * BANK1_FW_BASE: start of update region
 */
#define BANK0_FW_BASE  0x40000  /* 256 KB offset */


UINT32 ICompFwUpdateBspPrepare(UINT8 componentId)
{
    UINT32 offset = 0;

    if (componentId == 0x30)
        offset = BANK0_FW_BASE;

    FMC_Erase(offset);
    return 0;
}

UINT32 ICompFwUpdateBspWrite(UINT32 offset, UINT8* pData, UINT8 length, UINT8 componentId)
{
    if (componentId == 0x30)
        offset += BANK0_FW_BASE;

    uint32_t u32Addr[length / 4], u32Data[length / 4];
    uint32_t u32i;
    printf("ICompFwUpdateBspWrite offset=0x%x len=%d\n", offset, length);

    /* Pack bytes into 32-bit words (little-endian) */
    for (u32i = 0; u32i < length / 4; u32i++)
    {
        u32Data[u32i] = ((uint32_t)pData[u32i * 4 + 3] << 24) |
                        ((uint32_t)pData[u32i * 4 + 2] << 16) |
                        ((uint32_t)pData[u32i * 4 + 1] <<  8) |
                        ((uint32_t)pData[u32i * 4 + 0]);
    }

    for (u32i = 0; u32i < length / 4; u32i++)
    {
        u32Addr[u32i] = offset + 4 * u32i;

        if (FMC_Write(u32Addr[u32i], u32Data[u32i]) != 0)
        {
            printf("FMC_Write address 0x%x failed!\n", u32Addr[u32i]);
            return (UINT32)(-1);
        }
    }

    return 0;
}

UINT32 ICompFwUpdateBspRead(UINT32 offset, UINT8* pData, UINT16 length, UINT8 componentId)
{
    if (componentId == 0x30)
        offset += BANK0_FW_BASE;

    uint32_t u32i;

    for (u32i = 0; u32i < length / 4; u32i++)
    {
        uint32_t u32Data = FMC_Read(offset + u32i * 4);
        pData[u32i * 4 + 0] = (uint8_t)(u32Data & 0xFF);
        pData[u32i * 4 + 1] = (uint8_t)((u32Data >> 8)  & 0xFF);
        pData[u32i * 4 + 2] = (uint8_t)((u32Data >> 16) & 0xFF);
        pData[u32i * 4 + 3] = (uint8_t)((u32Data >> 24) & 0xFF);
    }

    return 0;
}

UINT32 ICompFwUpdateBspCalcCRC(UINT16 *pCRC, UINT8 componentId)
{
    uint32_t u32ChkSum;
    uint32_t write_len;

    if (componentId == 0x30)
        write_len = 32;
    else
        write_len = 0x10000;

    u32ChkSum = FMC_GetChkSum(BANK0_FW_BASE, write_len);
    *pCRC = (uint16_t)u32ChkSum;
    return 0;
}

INT32 ICompFwUpdateBspAuthenticateFWImage(void)
{
    return 0;  /* Assume pass */
}
