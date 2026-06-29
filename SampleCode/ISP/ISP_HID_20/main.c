/***************************************************************************//**
 * @file     main.c
 * @brief    ISP tool main function
 * @version  0x32
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @copyright Copyright (c) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include <stdio.h>
#include "NuMicro.h"
#include "hid_transfer.h"
#include "targetdev.h"

int32_t g_FMC_i32ErrCode = 0;

void ProcessHardFault(void);
void SH_Return(void);
void SendChar_ToUART(void);
int32_t SYS_Init(void);

void ProcessHardFault(void) {}
void SH_Return(void) {}
void SendChar_ToUART(void) {}

/* Add implementations to fix linker warnings from the newlib-nano C library in VSCode-GCC14.3.1 */
void _close(void) {}
void _lseek(void) {}
void _read_r(void) {}
void _write_r(void) {}

int32_t SYS_Init(void)
{
    uint32_t volatile i;
    uint32_t u32TimeOutCnt;

    /*---------------------------------------------------------------------------------------------------------*/
    /* Init System Clock                                                                                       */
    /*---------------------------------------------------------------------------------------------------------*/

    /* Enable HIRC and HXT clock */
    CLK->PWRCTL |= CLK_PWRCTL_HIRCEN_Msk | CLK_PWRCTL_HXTEN_Msk;

    /* Wait for HIRC and HXT clock ready */
    u32TimeOutCnt = SystemCoreClock; /* 1 second time-out */
    while(!(CLK->STATUS & (CLK_STATUS_HIRCSTB_Msk | CLK_STATUS_HXTSTB_Msk)))
    {
        if(--u32TimeOutCnt == 0)
            return -1;
    }

    /* Set HCLK clock source as HIRC first */
    CLK->CLKSEL0 = (CLK->CLKSEL0 & (~CLK_CLKSEL0_HCLKSEL_Msk)) | CLK_CLKSEL0_HCLKSEL_HIRC;

    /* Disable PLL clock before setting PLL frequency */
    CLK->PLLCTL |= CLK_PLLCTL_PD_Msk;

    /* Set PLL clock as 180MHz from HIRC/4 */
    CLK->PLLCTL = CLK_PLLCTL_180MHz_HIRC_DIV4;

    /* Wait for PLL clock ready */
    u32TimeOutCnt = SystemCoreClock; /* 1 second time-out */
    while(!(CLK->STATUS & CLK_STATUS_PLLSTB_Msk))
    {
        if(--u32TimeOutCnt == 0)
            return -1;
    }

    /* Set power level by HCLK working frequency */
    SYS->PLCTL = (SYS->PLCTL & (~SYS_PLCTL_PLSEL_Msk)) | SYS_PLCTL_PLSEL_PL0;

    /* Set flash access cycle by HCLK working frequency */
    FMC->CYCCTL = (FMC->CYCCTL & (~FMC_CYCCTL_CYCLE_Msk)) | (8);

    /* Set PCLK0 and PCLK1 to HCLK/2 */
    CLK->PCLKDIV = (CLK_PCLKDIV_APB0DIV_DIV2 | CLK_PCLKDIV_APB1DIV_DIV2);

    /* Select HCLK clock source as PLL and HCLK source divider as 1 */
    CLK->CLKDIV0 = (CLK->CLKDIV0 & (~CLK_CLKDIV0_HCLKDIV_Msk)) | CLK_CLKDIV0_HCLK(1);
    CLK->CLKSEL0 = (CLK->CLKSEL0 & (~CLK_CLKSEL0_HCLKSEL_Msk)) | CLK_CLKSEL0_HCLKSEL_PLL;

    /* Update System Core Clock */
    PllClock        = FREQ_180MHZ;
    SystemCoreClock = FREQ_180MHZ;
    CyclesPerUs     = SystemCoreClock / 1000000;  /* For CLK_SysTickDelay() */

    /* Select HSUSBD */
    SYS->USBPHY &= ~SYS_USBPHY_HSUSBROLE_Msk;

    /* Enable USB PHY */
    SYS->USBPHY = (SYS->USBPHY & ~(SYS_USBPHY_HSUSBROLE_Msk | SYS_USBPHY_HSUSBACT_Msk)) | SYS_USBPHY_HSUSBEN_Msk;
    for(i = 0; i < 0x1000; i++);   // delay > 10 us
    SYS->USBPHY |= SYS_USBPHY_HSUSBACT_Msk;

    /* Enable HSUSBD module clock */
    CLK->AHBCLK0 |= CLK_AHBCLK0_HSUSBDCKEN_Msk;

#if 0
    /* Enable GPIO B clock */
    CLK->AHBCLK0 |= CLK_AHBCLK0_GPBCKEN_Msk;

    /* Enable UART0 for debug output (PB12=RXD, PB13=TXD) */
		
    CLK->APBCLK0 |= CLK_APBCLK0_UART0CKEN_Msk;
    CLK->CLKSEL1 = (CLK->CLKSEL1 & ~CLK_CLKSEL1_UART0SEL_Msk) | CLK_CLKSEL1_UART0SEL_HIRC;
    CLK->CLKDIV0 = (CLK->CLKDIV0 & ~CLK_CLKDIV0_UART0DIV_Msk) | CLK_CLKDIV0_UART0(1);
    SET_UART0_RXD_PB12();
    SET_UART0_TXD_PB13();
#endif
    return 0;
}

/*---------------------------------------------------------------------------------------------------------*/
/* AP boot slot layout (per 128 KB bank, matching gen_checksum_bin.py)                                     */
/*   bank_base + 0x1FFF0  [4B]  FW_VERSION                                                                 */
/*   bank_base + 0x1FFF8  [4B]  FW_CHECKSUM — 32-bit byte-sum of entire 128 KB image                      */
/*                               (checksum slot bytes treated as 0x00 during computation)                  */
/*---------------------------------------------------------------------------------------------------------*/
#define AP0_BASE       0x00000UL
#define AP1_BASE       0x20000UL
#define AP_BANK_SIZE   0x20000UL   /* 128 KB per bank */
#define AP_VER_OFFSET  0x1FFF0UL   /* FW_VERSION offset from bank start */
#define AP_CHK_OFFSET  0x1FFF8UL   /* FW_CHECKSUM offset from bank start */

/**
 * Boot_ChecksumOK — returns 1 if the 32-bit byte-sum checksum stored at
 * (u32Base + AP_CHK_OFFSET) matches the computed byte-sum of the bank;
 * returns 0 for an erased slot (0xFFFFFFFF) or a mismatch.
 */
static int Boot_ChecksumOK(uint32_t u32Base)
{
    uint32_t u32Addr;
    uint32_t u32Sum     = 0;
    uint32_t u32ChkAddr = u32Base + AP_CHK_OFFSET;
    uint32_t u32Stored  = 0;

    FMC_Read_User(u32ChkAddr, &u32Stored);

    /* 0xFFFFFFFF means the bank is erased / unprogrammed */
    if (u32Stored == 0xFFFFFFFFUL)
        return 0;

    /* Sum every byte from the bank start up to (but not including) the
     * checksum slot. This matches the range used by gen_checksum_bin.py
     * (sum of bytes 0x00000 .. AP_CHK_OFFSET-1). */
    for (u32Addr = u32Base; u32Addr < u32ChkAddr; u32Addr += 4)
    {
        uint32_t u32Word = 0;
        FMC_Read_User(u32Addr, &u32Word);
        u32Sum  += (u32Word        & 0xFFUL);
        u32Sum  += ((u32Word >>  8) & 0xFFUL);
        u32Sum  += ((u32Word >> 16) & 0xFFUL);
        u32Sum  += ((u32Word >> 24) & 0xFFUL);
    }
    u32Sum &= 0xFFFFFFFFUL;

    return (u32Sum == u32Stored) ? 1 : 0;
}

/**
 * Boot_JumpToAP — set VECMAP to u32Addr, clear BS (APROM boot) and
 * trigger a system reset.  Mirrors CMD_RUN_APROM in isp_user.c.
 * Never returns.
 */
static void Boot_JumpToAP(uint32_t u32Addr)
{
    FMC_SetVectorPageAddr(u32Addr);
    NVIC_SystemReset();
    while (1);  /* unreachable — suppress "no return" warnings */
}

/*---------------------------------------------------------------------------------------------------------*/
/*  Main Function                                                                                          */
/*---------------------------------------------------------------------------------------------------------*/
int main(void)
{
    /* Unlock write-protected registers */
    SYS_UnlockReg();

    /* Init System, peripheral clock and multi-function I/O */
    SYS_Init();

    /* Init UART0 for debug output */
   // UART_Open(UART0, 115200);


    /* Enable ISP */
    CLK->AHBCLK0 |= CLK_AHBCLK0_ISPCKEN_Msk;
    FMC->ISPCTL |= FMC_ISPCTL_ISPEN_Msk | FMC_ISPCTL_APUEN_Msk | FMC_ISPCTL_DFUEN_Msk | FMC_ISPCTL_ISPFF_Msk;

    /* Get APROM and Data Flash size */
    g_u32ApromSize = 128*1024;
    g_u32DataFlashAddr = 0x20000;
    g_u32DataFlashSize = 128*1024;

    /* ----------------------------------------------------------------------- *
     * Boot decision: verify AP0 and AP1 checksums (gen_checksum_bin.py         *
     * 32-bit byte-sum) and compare FW_VERSION values.                          *
     *  - Both valid   → boot the bank with the higher (newer) version.         *
     *  - One valid    → boot that bank.                                         *
     *  - Both invalid → fall through to USB ISP update below (line ~125).      *
     * ----------------------------------------------------------------------- */
    {
        int      ap0_ok = Boot_ChecksumOK(AP0_BASE);
        int      ap1_ok = Boot_ChecksumOK(AP1_BASE);
        uint32_t ver0   = 0;
        uint32_t ver1   = 0;
        uint32_t chk0   = 0;
        uint32_t chk1   = 0;
        FMC_Read_User(AP0_BASE + AP_VER_OFFSET, &ver0);
        FMC_Read_User(AP1_BASE + AP_VER_OFFSET, &ver1);
        FMC_Read_User(AP0_BASE + AP_CHK_OFFSET, &chk0);
        FMC_Read_User(AP1_BASE + AP_CHK_OFFSET, &chk1);
#if 0
        printf("[LDROM] AP0: ok=%d  ver=0x%08X  chk=0x%08X\n", ap0_ok, ver0, chk0);
        printf("[LDROM] AP1: ok=%d  ver=0x%08X  chk=0x%08X\n", ap1_ok, ver1, chk1);
#endif
        if (ap0_ok && ap1_ok)
        {
            /* Both valid: boot the bank with the higher (newer) version.
             * Treat 0xFFFFFFFF (erased slot) as version 0 so it is never
             * preferred over a real version.
             * If versions are equal, prefer AP1 (most recently updated). */
            uint32_t cmp0 = (ver0 == 0xFFFFFFFFUL) ? 0UL : ver0;
            uint32_t cmp1 = (ver1 == 0xFFFFFFFFUL) ? 0UL : ver1;
            Boot_JumpToAP((cmp1 >= cmp0) ? AP1_BASE : AP0_BASE);
        }
        else if (ap0_ok)
        {
            Boot_JumpToAP(AP0_BASE);
        }
        else if (ap1_ok)
        {
            Boot_JumpToAP(AP1_BASE);
        }
        /* else: both checksums invalid — fall through to USB ISP update */
    }

    /* Open HSUSBD controller */
    HSUSBD_Open(NULL, NULL, NULL);

    /* Endpoint configuration */
    HID_Init();

    /* Enable HSUSBD interrupt */
    // NVIC_EnableIRQ(USBD20_IRQn);

    /* Start transaction */
    HSUSBD->OPER = HSUSBD_OPER_HISPDEN_Msk;   /* high-speed */
    HSUSBD_CLR_SE0();

    while(1)
    {
        /* Polling HSUSBD interrupt flag */
        USBD20_IRQHandler();

        if(g_u8UsbDataReady == TRUE)
        {
            ParseCmd((uint8_t *)g_u8UsbRcvBuff, 64);
            EPA_Handler();
            g_u8UsbDataReady = FALSE;
        }
    }



    /* Trap the CPU */
    while(1);
}
