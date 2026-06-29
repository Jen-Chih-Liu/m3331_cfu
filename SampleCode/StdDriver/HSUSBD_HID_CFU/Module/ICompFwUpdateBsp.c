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
#include "hid_transfer.h"

/* Flash layout:
 *   AP0: 0x00000 - 0x1FFFF (128 KB)  → componentId 0x30
 *   AP1: 0x20000 - 0x3FFFF (128 KB)  → componentId 0x31
 *
 * ACTIVE_BANK is defined in the Keil target preprocessor:
 *   Target "AP0": ACTIVE_BANK=0  → CFU writes to AP1 (inactive)
 *   Target "AP1": ACTIVE_BANK=1  → CFU writes to AP0 (inactive)
 */
#define AP0_BASE   0x00000U   /* AP0 flash start */
#define AP1_BASE   0x20000U   /* AP1 flash start */
#define AP_SIZE    0x20000U   /* 128 KB per bank  */

#ifndef ACTIVE_BANK
#define ACTIVE_BANK 0
#endif

#if ACTIVE_BANK == 0
#define TARGET_FW_BASE  AP1_BASE  /* Running AP0 → update destination is AP1 */
#else
#define TARGET_FW_BASE  AP0_BASE  /* Running AP1 → update destination is AP0 */
#endif


UINT32 ICompFwUpdateBspPrepare(UINT8 componentId)
{
    UINT32 addr;
    (void)componentId;

    /* Erase every 4 KB page in the INACTIVE bank */
    for (addr = TARGET_FW_BASE; addr < TARGET_FW_BASE + AP_SIZE; addr += FMC_FLASH_PAGE_SIZE)
    {
        if (FMC_Erase(addr) != 0)
        {
            CFU_DBG("FMC_Erase 0x%x failed!\n", addr);
            return (UINT32)(-1);
        }
    }
    return 0;
}

UINT32 ICompFwUpdateBspWrite(UINT32 offset, UINT8* pData, UINT8 length, UINT8 componentId)
{
    (void)componentId;
    offset += TARGET_FW_BASE;  /* absolute address in inactive bank */

    uint32_t u32i;
    uint32_t u32WordCount = length / 4;   /* number of 32-bit words */
    CFU_DBG("ICompFwUpdateBspWrite offset=0x%x len=%d\n", offset, length);

    /* Write 8 bytes (2 words) at a time using FMC_Write8Bytes.
     * length must be a multiple of 8; if odd word remains, write it with FMC_Write. */
    for (u32i = 0; u32i + 1 < u32WordCount; u32i += 2)
    {
        uint32_t u32Addr = offset + u32i * 4;
        uint32_t u32Lo = ((uint32_t)pData[u32i * 4 + 3] << 24) |
                         ((uint32_t)pData[u32i * 4 + 2] << 16) |
                         ((uint32_t)pData[u32i * 4 + 1] <<  8) |
                         ((uint32_t)pData[u32i * 4 + 0]);
        uint32_t u32Hi = ((uint32_t)pData[(u32i + 1) * 4 + 3] << 24) |
                         ((uint32_t)pData[(u32i + 1) * 4 + 2] << 16) |
                         ((uint32_t)pData[(u32i + 1) * 4 + 1] <<  8) |
                         ((uint32_t)pData[(u32i + 1) * 4 + 0]);

        if (FMC_Write8Bytes(u32Addr, u32Lo, u32Hi) != 0)
        {
            CFU_DBG("FMC_Write8Bytes address 0x%x failed!\n", u32Addr);
            return (UINT32)(-1);
        }
    }

    /* Handle trailing 4-byte word if length is not a multiple of 8 */
    if (u32WordCount & 1u)
    {
        uint32_t u32Addr = offset + (u32WordCount - 1) * 4;
        uint32_t u32Data = ((uint32_t)pData[(u32WordCount - 1) * 4 + 3] << 24) |
                           ((uint32_t)pData[(u32WordCount - 1) * 4 + 2] << 16) |
                           ((uint32_t)pData[(u32WordCount - 1) * 4 + 1] <<  8) |
                           ((uint32_t)pData[(u32WordCount - 1) * 4 + 0]);
        if (FMC_Write(u32Addr, u32Data) != 0)
        {
            CFU_DBG("FMC_Write address 0x%x failed!\n", u32Addr);
            return (UINT32)(-1);
        }
    }

    /* Handle trailing 1–3 bytes that do not fill a complete 4-byte word.
     * Pad unused bytes with 0xFF (erased-state) so they do not corrupt flash. */
    {
        uint32_t byteRemainder = (uint32_t)length % 4u;
        if (byteRemainder != 0u)
        {
            uint32_t u32Addr = offset + ((uint32_t)length & ~3u);
            uint32_t u32Data = 0xFFFFFFFFu;
            uint32_t j;
            for (j = 0; j < byteRemainder; j++)
                u32Data = (u32Data & ~(0xFFu << (j * 8u))) |
                          ((uint32_t)pData[((uint32_t)length & ~3u) + j] << (j * 8u));
            if (FMC_Write(u32Addr, u32Data) != 0)
            {
                CFU_DBG("FMC_Write (remainder) address 0x%x failed!\n", u32Addr);
                return (UINT32)(-1);
            }
        }
    }

    return 0;
}

UINT32 ICompFwUpdateBspRead(UINT32 offset, UINT8* pData, UINT16 length, UINT8 componentId)
{
    (void)componentId;
    offset += TARGET_FW_BASE;

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
    (void)componentId;

    /* Checksum the inactive bank (TARGET_FW_BASE, 128 KB) */
    u32ChkSum = FMC_GetChkSum(TARGET_FW_BASE, AP_SIZE);
    *pCRC = (uint16_t)u32ChkSum;
    return 0;
}

INT32 ICompFwUpdateBspAuthenticateFWImage(void)
{
    return 0;  /* Assume pass */
}
