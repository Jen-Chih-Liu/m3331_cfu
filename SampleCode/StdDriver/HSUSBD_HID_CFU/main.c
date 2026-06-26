/******************************************************************************
 * @file     main.c
 * @brief    m3331 HSUSBD CFU HID Transfer Sample Code
 *           Demonstrates Component Firmware Update (CFU) over USB HID
 *           on the m3331 (Cortex-M33) platform using the HSUSBD controller.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/

#include <stdio.h>
#include "NuMicro.h"
#include "hid_transfer.h"
#include "ComponentFwUpdate.h"
#include "IComponentFirmwareUpdate.h"

/*---------------------------------------------------------------------------------------------------------*/
/* Globals                                                                                                 */
/*---------------------------------------------------------------------------------------------------------*/
/* Component registrations defined in ComponentFwUpdate.c */
extern volatile COMPONENT_REGISTRATION s_Comp_Registration;
extern volatile COMPONENT_REGISTRATION s_Comp_Registration_2;

/*---------------------------------------------------------------------------------------------------------*/
/* Function prototypes                                                                                     */
/*---------------------------------------------------------------------------------------------------------*/
void SYS_Init(void);
void UART0_Init(void);

unsigned long FirmwareUpdateInit(void);
void IComponentFirmwareUpdateRegisterComponent(COMPONENT_REGISTRATION *pRegistration);

/*---------------------------------------------------------------------------------------------------------*/
/*  System Clock Initialisation                                                                            */
/*---------------------------------------------------------------------------------------------------------*/
void SYS_Init(void)
{
    uint32_t volatile i;

    /* Unlock protected registers */
    SYS_UnlockReg();

    /* Enable HIRC and HXT clock */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk | CLK_PWRCTL_HXTEN_Msk);

    /* Wait for HIRC and HXT clock ready */
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk | CLK_STATUS_HXTSTB_Msk);

    /* Set PCLK0 and PCLK1 to HCLK/2 */
    CLK->PCLKDIV = (CLK_PCLKDIV_APB0DIV_DIV2 | CLK_PCLKDIV_APB1DIV_DIV2);

    /* Set core clock to 180 MHz */
    CLK_SetCoreClock(FREQ_180MHZ);

    /* Enable all GPIO clocks */
    CLK->AHBCLK0 |= CLK_AHBCLK0_GPACKEN_Msk | CLK_AHBCLK0_GPBCKEN_Msk |
                    CLK_AHBCLK0_GPCCKEN_Msk | CLK_AHBCLK0_GPDCKEN_Msk  |
                    CLK_AHBCLK0_GPECKEN_Msk | CLK_AHBCLK0_GPFCKEN_Msk  |
                    CLK_AHBCLK0_GPGCKEN_Msk | CLK_AHBCLK0_GPHCKEN_Msk;

    /* Enable UART0 module clock */
    CLK_EnableModuleClock(UART0_MODULE);
    CLK_SetModuleClock(UART0_MODULE, CLK_CLKSEL1_UART0SEL_HIRC, CLK_CLKDIV0_UART0(1));

    /* Enable TIMER0 module clock (used by CFU timer) */
    CLK_EnableModuleClock(TMR0_MODULE);
    CLK_SetModuleClock(TMR0_MODULE, CLK_CLKSEL1_TMR0SEL_HIRC, 0);

    /* Select HSUSBD and enable USB PHY */
    SYS->USBPHY &= ~SYS_USBPHY_HSUSBROLE_Msk;
    SYS->USBPHY = (SYS->USBPHY &
                   ~(SYS_USBPHY_HSUSBROLE_Msk | SYS_USBPHY_HSUSBACT_Msk)) |
                  SYS_USBPHY_HSUSBEN_Msk;
    for (i = 0; i < 0x1000; i++);  /* Delay > 10 us for PHY stabilisation */
    SYS->USBPHY |= SYS_USBPHY_HSUSBACT_Msk;

    /* Enable HSUSBD module clock */
    CLK_EnableModuleClock(HSUSBD_MODULE);

    /*---------------------------------------------------------------------------------------------------------*/
    /* Init I/O Multi-function                                                                                 */
    /*---------------------------------------------------------------------------------------------------------*/
    /* UART0 multi-function pins */
    SET_UART0_RXD_PB12();
    SET_UART0_TXD_PB13();

    /* Lock protected registers */
    SYS_LockReg();
}

void UART0_Init(void)
{
    SYS_ResetModule(UART0_RST);
    UART_Open(UART0, 115200);
}

/*---------------------------------------------------------------------------------------------------------*/
/*  Main Function                                                                                          */
/*---------------------------------------------------------------------------------------------------------*/
int32_t main(void)
{
    /* Unlock protected registers */
    SYS_UnlockReg();

    /* Init system, peripheral clock and multi-function I/O */
    SYS_Init();

    /* Init UART for debug output */
    UART0_Init();

    printf("\n");
    printf("+--------------------------------------------------------+\n");
    printf("|  NuMicro m3331 HSUSBD CFU + HID Transfer Sample Code   |\n");
    printf("+--------------------------------------------------------+\n");

    /* Open HSUSBD and register HID class request handler */
    HSUSBD_Open(&gsHSInfo, HID_ClassRequest, NULL);
    HSUSBD_SetVendorRequest(HID_VendorRequest);

    /* Endpoint configuration */
    HID_Init();

    /* Enable HSUSBD interrupt */
    NVIC_EnableIRQ(USBD20_IRQn);

    /* Start transaction */
    HSUSBD_Start();

    /* Enable FMC ISP function */
    SYS_UnlockReg();
    FMC_Open();
    FMC_ENABLE_AP_UPDATE();

    /* Initialise CFU engine */
    FirmwareUpdateInit();

    /* Register component chain */
    IComponentFirmwareUpdateRegisterComponent((COMPONENT_REGISTRATION *)&s_Comp_Registration);
    IComponentFirmwareUpdateRegisterComponent((COMPONENT_REGISTRATION *)&s_Comp_Registration_2);

    /* Build initial FW version response */
    ProcessCFWUGetFWVersion((void *)s_Get_FwVer_Resp);

    while (1)
    {
        /* Main loop: CFU processing is driven by USB interrupts */
    }
}
