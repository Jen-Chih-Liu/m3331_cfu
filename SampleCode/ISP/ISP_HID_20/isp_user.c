/***************************************************************************//**
 * @file     isp_user.c
 * @brief    ISP command source file
 * @version  0x32
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @copyright Copyright (c) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include <stdio.h>
#include <string.h>
#include "isp_user.h"

__attribute__((aligned(4))) uint8_t g_au8ResponseBuff[64];
__attribute__((aligned(4))) static uint8_t g_au8ApromBuf[FMC_FLASH_PAGE_SIZE];
uint32_t g_u32UpdateApromCmd;
uint32_t g_u32ApromSize, g_u32DataFlashAddr, g_u32DataFlashSize;

static uint16_t Checksum(unsigned char *buf, int len)
{
    int i;
    uint16_t c;

    for(c = 0, i = 0 ; i < len; i++)
    {
        c += buf[i];
    }

    return (c);
}

static uint16_t CalCheckSum(uint32_t u32start, uint32_t u32len)
{
    uint32_t u32i;
    uint16_t u16lcksum = 0;

    for(u32i = 0; u32i < u32len; u32i += FMC_FLASH_PAGE_SIZE)
    {
        ReadData(u32start + u32i, u32start + u32i + FMC_FLASH_PAGE_SIZE, (uint32_t *)g_au8ApromBuf);

        if(u32len - u32i >= FMC_FLASH_PAGE_SIZE)
        {
            u16lcksum += Checksum(g_au8ApromBuf, FMC_FLASH_PAGE_SIZE);
        }
        else
        {
            u16lcksum += Checksum(g_au8ApromBuf, u32len - u32i);
        }
    }

    return u16lcksum;
}

/*---------------------------------------------------------------------------------------------------------*/
/* AP boot slot layout (mirrors main.c / gen_checksum_bin.py)                                              */
/*   AP0 0x00000-0x1EFFF | AP1 0x1F000-0x3DFFF (each 124 KB)                                               */
/*   bank_base + 0x1EFF0 [4B] FW_VERSION | bank_base + 0x1EFF8 [4B] FW_CHECKSUM (32-bit byte-sum)          */
/*---------------------------------------------------------------------------------------------------------*/
#define AP0_BASE        0x00000UL
#define AP1_BASE        0x1F000UL
#define AP_VER_OFFSET   0x1EFF0UL
#define AP_CHK_OFFSET   0x1EFF8UL

/* Returns 1 if the 32-bit byte-sum stored at (u32Base + AP_CHK_OFFSET) matches
 * the computed byte-sum of the bank (checksum slot excluded); 0 otherwise. */
static int RunApChecksumOK(uint32_t u32Base)
{
    uint32_t u32Addr;
    uint32_t u32Sum     = 0;
    uint32_t u32ChkAddr = u32Base + AP_CHK_OFFSET;
    uint32_t u32Stored  = 0;

    FMC_Read_User(u32ChkAddr, &u32Stored);

    if(u32Stored == 0xFFFFFFFFUL)
        return 0;

    for(u32Addr = u32Base; u32Addr < u32ChkAddr; u32Addr += 4)
    {
        uint32_t u32Word = 0;
        FMC_Read_User(u32Addr, &u32Word);
        u32Sum += (u32Word         & 0xFFUL);
        u32Sum += ((u32Word >>  8)  & 0xFFUL);
        u32Sum += ((u32Word >> 16)  & 0xFFUL);
        u32Sum += ((u32Word >> 24)  & 0xFFUL);
    }

    return (u32Sum == u32Stored) ? 1 : 0;
}

/* Selects the AP bank to run, mirroring the power-on boot decision in main():
 * both valid → newer FW_VERSION (tie → AP1); one valid → that bank;
 * none valid → AP0 (best effort). */
static uint32_t RunApSelectBank(void)
{
    int      ap0_ok = RunApChecksumOK(AP0_BASE);
    int      ap1_ok = RunApChecksumOK(AP1_BASE);
    uint32_t ver0   = 0;
    uint32_t ver1   = 0;

    if(ap0_ok && ap1_ok)
    {
        FMC_Read_User(AP0_BASE + AP_VER_OFFSET, &ver0);
        FMC_Read_User(AP1_BASE + AP_VER_OFFSET, &ver1);
        if(ver0 == 0xFFFFFFFFUL) ver0 = 0;
        if(ver1 == 0xFFFFFFFFUL) ver1 = 0;
        return (ver1 >= ver0) ? AP1_BASE : AP0_BASE;
    }
    else if(ap1_ok)
    {
        return AP1_BASE;
    }

    return AP0_BASE;
}

int ParseCmd(uint8_t *pu8Buffer, uint8_t u8len)
{
    static uint32_t u32StartAddress, u32StartAddress_bak, u32TotalLen, u32TotalLen_bak, u32LastDataLen, u32PackNo = 1;
    uint32_t u32PageAddress;
    uint8_t *pu8Response;
    uint16_t u16Lcksum;
    uint32_t u32Lcmd, u32srclen, u32i, u32regcnf0, u32security;
    uint8_t *pu8Src;
    static uint32_t u32Gcmd;
    pu8Response = g_au8ResponseBuff;
    pu8Src = pu8Buffer;
    u32srclen = u8len;
    u32Lcmd = inpw((uint32_t)pu8Src);
    outpw((uint32_t)(pu8Response + 4), 0);
    pu8Src += 8;
    u32srclen -= 8;
    ReadData(Config0, Config0 + 16, (uint32_t *)(uint32_t)(pu8Response + 8)); /* Read config */
    u32regcnf0 = *(uint32_t *)(pu8Response + 8);
    u32security = u32regcnf0 & 0x2;

    if(u32Lcmd == CMD_SYNC_PACKNO)
    {
        u32PackNo = inpw((uint32_t)pu8Src);
    }

    if((u32Lcmd) && (u32Lcmd != CMD_RESEND_PACKET))
    {
        u32Gcmd = u32Lcmd;
    }

    if(u32Lcmd == CMD_GET_FWVER)
    {
        pu8Response[8] = FW_VERSION; /* version 2.3 */
    }
    else if(u32Lcmd == CMD_GET_DEVICEID)
    {
        outpw((uint32_t)(pu8Response + 8), SYS->PDID);
        goto out;
    }
    else if(u32Lcmd == CMD_READ_APROM)
    {
        /* Read READ_APROM_CHUNK bytes of APROM starting at the address in the
         * request payload ([8..11]) into the response data area ([8..]).
         * The host iterates over the whole image to dump it. */
        u32PageAddress = inpw((uint32_t)pu8Src);
        ReadData(u32PageAddress, u32PageAddress + READ_APROM_CHUNK,
                 (uint32_t *)(uint32_t)(pu8Response + 8));
        goto out;
    }
    else if(u32Lcmd == CMD_RUN_APROM)
    {
        /* Jump into the application the same way the power-on boot path does
         * (see Boot_JumpToAP in main.c): remap the vector page to the selected
         * AP bank and issue a system reset.
         *
         * A plain software reset (clearing BS + SYSRESETREQ) would re-enter the
         * LDROM, which then stays in the ISP loop because the POR/PIN reset
         * flags are clear — see the "RSTSTS & 0x3 == 0 -> _LDROM_LOOP"
         * short-circuit in main().  Remapping VECMAP instead makes the CPU boot
         * straight into the AP reset vector, bypassing the LDROM entirely. */
        FMC_SetVectorPageAddr(RunApSelectBank());
        NVIC_SystemReset();

        /* Trap the CPU */
        while(1);
    }
    else if(u32Lcmd == CMD_RUN_LDROM || u32Lcmd == CMD_RESET)
    {
        /* Clear POR and Reset Pin reset flag */
        SYS_CLEAR_RST_SOURCE(SYS_RSTSTS_PORF_Msk);
        SYS_CLEAR_RST_SOURCE(SYS_RSTSTS_PINRF_Msk);

        /* Set BS */
        if(u32Lcmd == CMD_RUN_LDROM)
        {
            u32i = (FMC->ISPCTL & 0xFFFFFFFC);
            u32i |= 0x00000002;
        }
        else
        {
            u32i = (FMC->ISPCTL & 0xFFFFFFFE); /* ISP disable */
        }

        outpw(&FMC->ISPCTL, u32i);
        outpw(&SCB->AIRCR, (V6M_AIRCR_VECTKEY_DATA | V6M_AIRCR_SYSRESETREQ));

        /* Trap the CPU */
        while(1);
    }
    else if(u32Lcmd == CMD_CONNECT)
    {
        u32PackNo = 1;
        goto out;
    }
    else if((u32Lcmd == CMD_UPDATE_APROM) || (u32Lcmd == CMD_ERASE_ALL))
    {
        EraseAP(FMC_APROM_BASE, (g_u32ApromSize < g_u32DataFlashAddr) ? g_u32ApromSize : g_u32DataFlashAddr);

        if(u32Lcmd == CMD_ERASE_ALL)    /* Erase data flash */
        {
            EraseAP(g_u32DataFlashAddr, g_u32DataFlashSize);
            *(uint32_t *)(pu8Response + 8) = u32regcnf0 | 0x02;
            UpdateConfig((uint32_t *)(pu8Response + 8), NULL);
        }

        g_u32UpdateApromCmd = TRUE;
    }
    else if(u32Lcmd == CMD_GET_FLASHMODE)
    {
        /* Return 1: APROM, 2: LDROM */
        outpw(pu8Response + 8, (FMC->ISPCTL & 0x2) ? 2 : 1);
    }

    if((u32Lcmd == CMD_UPDATE_APROM) || (u32Lcmd == CMD_UPDATE_DATAFLASH))
    {
        if(u32Lcmd == CMD_UPDATE_DATAFLASH)
        {
            u32StartAddress = g_u32DataFlashAddr;

            if(g_u32DataFlashSize)
            {
                EraseAP(g_u32DataFlashAddr, g_u32DataFlashSize);
            }
            else
            {
                goto out;
            }
        }
        else
        {
            u32StartAddress = 0;
        }

        u32TotalLen = inpw(pu8Src + 4);
        pu8Src += 8;
        u32srclen -= 8;
        u32StartAddress_bak = u32StartAddress;
        u32TotalLen_bak = u32TotalLen;
    }
    #if 0
    else if(u32Lcmd == CMD_UPDATE_CONFIG)
    {
        if((u32security == 0) && (!g_u32UpdateApromCmd))    /* security lock */
        {
            goto out;
        }

        UpdateConfig((uint32_t *)(uint32_t)pu8Src, (uint32_t *)(uint32_t)(pu8Response + 8));
        goto out;
    }
    else if(u32Lcmd == CMD_RESEND_PACKET)      /* for APROM and Data flash only */
    {
        u32StartAddress -= u32LastDataLen;
        u32TotalLen += u32LastDataLen;
        u32PageAddress = u32StartAddress & (0x100000 - FMC_FLASH_PAGE_SIZE);

        if(u32PageAddress >= Config0)
        {
            goto out;
        }

        ReadData(u32PageAddress, u32StartAddress, (uint32_t *)g_au8ApromBuf);
        FMC_Erase_User(u32PageAddress);
        WriteData(u32PageAddress, u32StartAddress, (uint32_t *)g_au8ApromBuf);

        if((u32StartAddress % FMC_FLASH_PAGE_SIZE) >= (FMC_FLASH_PAGE_SIZE - u32LastDataLen))
        {
            FMC_Erase_User(u32PageAddress + FMC_FLASH_PAGE_SIZE);
        }

        goto out;
    }
    #endif
    if((u32Gcmd == CMD_UPDATE_APROM) || (u32Gcmd == CMD_UPDATE_DATAFLASH))
    {
        if(u32TotalLen < u32srclen)
        {
            u32srclen = u32TotalLen; /* Prevent last package from over writing */
        }

        u32TotalLen -= u32srclen;
        WriteData(u32StartAddress, u32StartAddress + u32srclen, (uint32_t *)(uint32_t)pu8Src);
        memset(pu8Src, 0, u32srclen);
        ReadData(u32StartAddress, u32StartAddress + u32srclen, (uint32_t *)(uint32_t)pu8Src);
        u32StartAddress += u32srclen;
        u32LastDataLen = u32srclen;

        if(u32TotalLen == 0)
        {
            u16Lcksum = CalCheckSum(u32StartAddress_bak, u32TotalLen_bak);
            outps(pu8Response + 8, u16Lcksum);
        }
    }

out:
    u16Lcksum = Checksum(pu8Buffer, u8len);
    outps((uint32_t)pu8Response, u16Lcksum);
    ++u32PackNo;
    outpw((uint32_t)(pu8Response + 4), u32PackNo);
    u32PackNo++;
    return 0;
}
