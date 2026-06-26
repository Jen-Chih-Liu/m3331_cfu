/******************************************************************************
 * @file     hid_transfer.c
 * @brief    m3331 HSUSBD CFU HID transfer sample file
 *
 * @note
 * Copyright (C) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include "NuMicro.h"
#include "hid_transfer.h"
#include "hid_cfu.h"

uint8_t volatile g_u8Suspend = 0;

/* CFU IN buffer shared with ComponentFwUpdate.c */
uint8_t          g_u8CfuInBuf[64];
volatile uint32_t g_u32CfuInLen  = 0;
volatile uint8_t  g_u8CfuInReady = 0;

/* OUT buffer filled by EPB_Handler */
uint8_t g_u8OutBuff[EPB_MAX_PKT_SIZE] __attribute__((aligned(4))) = {0};

/* FW version response buffer (populated by ProcessCFWUGetFWVersion) */
volatile uint8_t s_Get_FwVer_Resp[60] =
{
    2, 0, 0, 2, /* header */

    2, 0, 0, 0, 0,  2, 0, 0, /* versionAndProductInfoBlob */
    2, 0, 0, 0, 0, 30, 0, 0,

    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/*--------------------------------------------------------------------------*/
/*  USBD20 Interrupt Handler (HSUSBD)                                        */
/*--------------------------------------------------------------------------*/
void USBD20_IRQHandler(void)
{
    __IO uint32_t IrqStL, IrqSt;

    IrqStL = HSUSBD->GINTSTS & HSUSBD->GINTEN;
    if (!IrqStL) return;

    /* USB bus event */
    if (IrqStL & HSUSBD_GINTSTS_USBIF_Msk)
    {
        IrqSt = HSUSBD->BUSINTSTS & HSUSBD->BUSINTEN;

        if (IrqSt & HSUSBD_BUSINTSTS_SOFIF_Msk)
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_SOFIF_Msk);

        if (IrqSt & HSUSBD_BUSINTSTS_RSTIF_Msk)
        {
            HSUSBD_SwReset();
            HSUSBD_ResetDMA();
            HSUSBD->EP[EPA].EPRSPCTL = HSUSBD_EPRSPCTL_FLUSH_Msk;
            HSUSBD->EP[EPB].EPRSPCTL = HSUSBD_EPRSPCTL_FLUSH_Msk;

            if (HSUSBD->OPER & 0x04)  /* high speed */
                HID_InitForHighSpeed();
            else                       /* full speed */
                HID_InitForFullSpeed();

            HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk);
            HSUSBD_SET_ADDR(0);
            HSUSBD_ENABLE_BUS_INT(HSUSBD_BUSINTEN_RSTIEN_Msk |
                                  HSUSBD_BUSINTEN_RESUMEIEN_Msk |
                                  HSUSBD_BUSINTEN_SUSPENDIEN_Msk |
                                  HSUSBD_BUSINTEN_VBUSDETIEN_Msk);
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_RSTIF_Msk);
            HSUSBD_CLR_CEP_INT_FLAG(0x1ffc);
        }

        if (IrqSt & HSUSBD_BUSINTSTS_RESUMEIF_Msk)
        {
            HSUSBD_ENABLE_BUS_INT(HSUSBD_BUSINTEN_RSTIEN_Msk |
                                  HSUSBD_BUSINTEN_SUSPENDIEN_Msk |
                                  HSUSBD_BUSINTEN_VBUSDETIEN_Msk);
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_RESUMEIF_Msk);
        }

        if (IrqSt & HSUSBD_BUSINTSTS_SUSPENDIF_Msk)
        {
            g_u8Suspend = 1;
            HSUSBD_ENABLE_BUS_INT(HSUSBD_BUSINTEN_RSTIEN_Msk |
                                  HSUSBD_BUSINTEN_RESUMEIEN_Msk |
                                  HSUSBD_BUSINTEN_VBUSDETIEN_Msk);
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_SUSPENDIF_Msk);
        }

        if (IrqSt & HSUSBD_BUSINTSTS_HISPDIF_Msk)
        {
            HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk);
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_HISPDIF_Msk);
        }

        if (IrqSt & HSUSBD_BUSINTSTS_DMADONEIF_Msk)
        {
            g_hsusbd_DmaDone = 1;
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_DMADONEIF_Msk);

            if (HSUSBD->DMACTL & HSUSBD_DMACTL_DMARD_Msk)
            {
                if (g_hsusbd_ShortPacket == 1)
                {
                    HSUSBD->EP[EPA].EPRSPCTL =
                        (HSUSBD->EP[EPA].EPRSPCTL & 0x10) | HSUSBD_EP_RSPCTL_SHORTTXEN;
                    g_hsusbd_ShortPacket = 0;
                }
            }
        }

        if (IrqSt & HSUSBD_BUSINTSTS_PHYCLKVLDIF_Msk)
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_PHYCLKVLDIF_Msk);

        if (IrqSt & HSUSBD_BUSINTSTS_VBUSDETIF_Msk)
        {
            if (HSUSBD_IS_ATTACHED())
            {
                /* USB Plug In */
                HSUSBD_ENABLE_USB();
                HSUSBD_ENABLE_HS_HANDSHAKE();
            }
            else
            {
                /* USB Un-plug */
                HSUSBD_DISABLE_USB();
                HSUSBD_DISABLE_HS_HANDSHAKE();
            }
            HSUSBD_CLR_BUS_INT_FLAG(HSUSBD_BUSINTSTS_VBUSDETIF_Msk);
        }
    }

    /* Control endpoint */
    if (IrqStL & HSUSBD_GINTSTS_CEPIF_Msk)
    {
        IrqSt = HSUSBD->CEPINTSTS & HSUSBD->CEPINTEN;

        if (IrqSt & HSUSBD_CEPINTSTS_SETUPTKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_SETUPTKIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_SETUPPKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_SETUPPKIF_Msk);
            HSUSBD_ProcessSetupPacket();
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_OUTTKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_OUTTKIF_Msk);
            HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_STSDONEIEN_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_INTKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_INTKIF_Msk);
            if (!(IrqSt & HSUSBD_CEPINTSTS_STSDONEIF_Msk))
            {
                HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_TXPKIF_Msk);
                HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_TXPKIEN_Msk);
                HSUSBD_CtrlIn();
            }
            else
            {
                HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_TXPKIF_Msk);
                HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_TXPKIEN_Msk |
                                      HSUSBD_CEPINTEN_STSDONEIEN_Msk);
            }
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_PINGIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_PINGIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_TXPKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
            HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_NAKCLR);
            if (g_hsusbd_CtrlInSize)
            {
                HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_INTKIF_Msk);
                HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_INTKIEN_Msk);
            }
            else
            {
                if (g_hsusbd_CtrlZero == 1)
                    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_ZEROLEN);
                HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
                HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk |
                                      HSUSBD_CEPINTEN_STSDONEIEN_Msk);
            }
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_TXPKIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_RXPKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_RXPKIF_Msk);
            HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_NAKCLR);
            HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk |
                                  HSUSBD_CEPINTEN_STSDONEIEN_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_NAKIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_NAKIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_STALLIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STALLIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_ERRIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_ERRIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_STSDONEIF_Msk)
        {
            HSUSBD_UpdateDeviceState();
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
            HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_BUFFULLIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_BUFFULLIF_Msk);
            return;
        }

        if (IrqSt & HSUSBD_CEPINTSTS_BUFEMPTYIF_Msk)
        {
            HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_BUFEMPTYIF_Msk);
            return;
        }
    }

    /* EPA: Interrupt IN */
    if (IrqStL & HSUSBD_GINTSTS_EPAIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPA].EPINTSTS & HSUSBD->EP[EPA].EPINTEN;
        if (HSUSBD->EP[EPA].EPINTSTS & HSUSBD_EPINTSTS_INTKIF_Msk)
            EPA_Handler();
        HSUSBD_CLR_EP_INT_FLAG(EPA, IrqSt);
    }

    /* EPB: Interrupt OUT */
    if (IrqStL & HSUSBD_GINTSTS_EPBIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPB].EPINTSTS & HSUSBD->EP[EPB].EPINTEN;
        if (HSUSBD->EP[EPB].EPINTSTS & HSUSBD_EPINTSTS_RXPKIF_Msk)
            EPB_Handler();
        HSUSBD_CLR_EP_INT_FLAG(EPB, IrqSt);
    }

    if (IrqStL & HSUSBD_GINTSTS_EPCIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPC].EPINTSTS & HSUSBD->EP[EPC].EPINTEN;
        HSUSBD_CLR_EP_INT_FLAG(EPC, IrqSt);
    }

    if (IrqStL & HSUSBD_GINTSTS_EPDIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPD].EPINTSTS & HSUSBD->EP[EPD].EPINTEN;
        HSUSBD_CLR_EP_INT_FLAG(EPD, IrqSt);
    }

    if (IrqStL & HSUSBD_GINTSTS_EPEIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPE].EPINTSTS & HSUSBD->EP[EPE].EPINTEN;
        HSUSBD_CLR_EP_INT_FLAG(EPE, IrqSt);
    }

    if (IrqStL & HSUSBD_GINTSTS_EPFIF_Msk)
    {
        IrqSt = HSUSBD->EP[EPF].EPINTSTS & HSUSBD->EP[EPF].EPINTEN;
        HSUSBD_CLR_EP_INT_FLAG(EPF, IrqSt);
    }
}

/*--------------------------------------------------------------------------*/
/*  EPA: Interrupt IN handler - called when host issues IN token              */
/*--------------------------------------------------------------------------*/
void EPA_Handler(void)
{
    CFU_SetInReport();
}

/*--------------------------------------------------------------------------*/
/*  EPB: Interrupt OUT handler - called when host sends OUT data              */
/*--------------------------------------------------------------------------*/
void EPB_Handler(void)
{
    uint32_t len, i;
    len = HSUSBD->EP[EPB].EPDATCNT & 0xffff;
    for (i = 0; i < len; i++)
        g_u8OutBuff[i] = HSUSBD->EP[EPB].EPDAT_BYTE;

    printf("EPB OUT len=%d\n", len);
    printf("===========================\n");
    for (i = 0; i < len; i++)
    {
        printf("0x%02x ", g_u8OutBuff[i]);
        if (i % 4 == 3 || i == len - 1) printf("\n");
    }
    printf("===========================\n");

    /* First byte is Report ID; skip it and pass the payload */
    if (len > 1)
        CFU_GetOutReport(g_u8OutBuff + 1, len - 1);
}

/*--------------------------------------------------------------------------*/
/**
  * @brief  Configure HSUSBD endpoints for CFU HID (high speed).
  */
void HID_InitForHighSpeed(void)
{
    /* EPA ==> Interrupt IN endpoint, address CFU_INT_IN_EP_NUM */
    HSUSBD_SetEpBufAddr(EPA, EPA_BUF_BASE, EPA_BUF_LEN);
    HSUSBD_SET_MAX_PAYLOAD(EPA, EPA_MAX_PKT_SIZE);
    HSUSBD_ConfigEp(EPA, CFU_INT_IN_EP_NUM, HSUSBD_EP_CFG_TYPE_INT, HSUSBD_EP_CFG_DIR_IN);

    /* EPB ==> Interrupt OUT endpoint, address CFU_INT_OUT_EP_NUM */
    HSUSBD_SetEpBufAddr(EPB, EPB_BUF_BASE, EPB_BUF_LEN);
    HSUSBD_SET_MAX_PAYLOAD(EPB, EPB_MAX_PKT_SIZE);
    HSUSBD_ConfigEp(EPB, CFU_INT_OUT_EP_NUM, HSUSBD_EP_CFG_TYPE_INT, HSUSBD_EP_CFG_DIR_OUT);
    HSUSBD_ENABLE_EP_INT(EPB, HSUSBD_EPINTEN_RXPKIEN_Msk | HSUSBD_EPINTEN_BUFFULLIEN_Msk);
}

void HID_InitForFullSpeed(void)
{
    /* EPA ==> Interrupt IN endpoint */
    HSUSBD_SetEpBufAddr(EPA, EPA_BUF_BASE, EPA_BUF_LEN);
    HSUSBD_SET_MAX_PAYLOAD(EPA, EPA_OTHER_MAX_PKT_SIZE);
    HSUSBD_ConfigEp(EPA, CFU_INT_IN_EP_NUM, HSUSBD_EP_CFG_TYPE_INT, HSUSBD_EP_CFG_DIR_IN);

    /* EPB ==> Interrupt OUT endpoint */
    HSUSBD_SetEpBufAddr(EPB, EPB_BUF_BASE, EPB_BUF_LEN);
    HSUSBD_SET_MAX_PAYLOAD(EPB, EPB_OTHER_MAX_PKT_SIZE);
    HSUSBD_ConfigEp(EPB, CFU_INT_OUT_EP_NUM, HSUSBD_EP_CFG_TYPE_INT, HSUSBD_EP_CFG_DIR_OUT);
    HSUSBD_ENABLE_EP_INT(EPB, HSUSBD_EPINTEN_RXPKIEN_Msk | HSUSBD_EPINTEN_BUFFULLIEN_Msk);
}

void HID_Init(void)
{
    /* Configure USB controller */
    /* Enable USB BUS, CEP, EPA, EPB global interrupts */
    HSUSBD_ENABLE_USB_INT(HSUSBD_GINTEN_USBIEN_Msk |
                          HSUSBD_GINTEN_CEPIEN_Msk  |
                          HSUSBD_GINTEN_EPAIEN_Msk  |
                          HSUSBD_GINTEN_EPBIEN_Msk);
    /* Enable BUS interrupts */
    HSUSBD_ENABLE_BUS_INT(HSUSBD_BUSINTEN_DMADONEIEN_Msk |
                          HSUSBD_BUSINTEN_RESUMEIEN_Msk   |
                          HSUSBD_BUSINTEN_RSTIEN_Msk      |
                          HSUSBD_BUSINTEN_VBUSDETIEN_Msk);
    /* Reset address to 0 */
    HSUSBD_SET_ADDR(0);

    /* Control endpoint */
    HSUSBD_SetEpBufAddr(CEP, CEP_BUF_BASE, CEP_BUF_LEN);
    HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_SETUPPKIEN_Msk |
                          HSUSBD_CEPINTEN_STSDONEIEN_Msk);

    HID_InitForHighSpeed();
    printf("HID_Init\n");
}

/*--------------------------------------------------------------------------*/
/*  HID Class Request handler                                                */
/*--------------------------------------------------------------------------*/
void HID_ClassRequest(void)
{
    if (gUsbCmd.bmRequestType & 0x80)   /* Device to host */
    {
        switch (gUsbCmd.bRequest)
        {
            case GET_REPORT:
            {
                /* Report Type = Feature (3) -> return FW version info.
                 * HID GET_REPORT response MUST include the Report ID as the
                 * first byte, followed by the report data. */
                if (((gUsbCmd.wValue >> 8) & 0xff) == HID_RPT_TYPE_FEATURE)
                {
                    static uint8_t s_featureBuf[1 + sizeof(s_Get_FwVer_Resp)];
                    s_featureBuf[0] = REPORT_ID_VERSIONS_FEATURE; /* 0x20 = 32 */
                    memcpy(s_featureBuf + 1, (uint8_t *)s_Get_FwVer_Resp,
                           sizeof(s_Get_FwVer_Resp));
                    HSUSBD_PrepareCtrlIn(s_featureBuf, sizeof(s_featureBuf));
                    HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_INTKIF_Msk);
                    HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_INTKIEN_Msk);
                }
                else
                {
                    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_STALLEN_Msk);
                }
                break;
            }
            case GET_IDLE:
            case GET_PROTOCOL:
            default:
                HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_STALLEN_Msk);
                break;
        }
    }
    else    /* Host to device */
    {
        switch (gUsbCmd.bRequest)
        {
            case SET_REPORT:
            {
                if (((gUsbCmd.wValue >> 8) & 0xff) == HID_RPT_TYPE_FEATURE)
                {
                    /* Feature report - acknowledge */
                    HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
                    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_NAKCLR);
                    HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_STSDONEIEN_Msk);
                }
                else if (((gUsbCmd.wValue >> 8) & 0xff) == HID_RPT_TYPE_OUTPUT)
                {
                    /* Output report - acknowledge */
                    HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
                    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_NAKCLR);
                    HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_STSDONEIEN_Msk);
                }
                else
                {
                    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_STALLEN_Msk);
                }
                break;
            }
            case SET_IDLE:
            {
                HSUSBD_CLR_CEP_INT_FLAG(HSUSBD_CEPINTSTS_STSDONEIF_Msk);
                HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_NAKCLR);
                HSUSBD_ENABLE_CEP_INT(HSUSBD_CEPINTEN_STSDONEIEN_Msk);
                break;
            }
            case SET_PROTOCOL:
            default:
                HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_STALLEN_Msk);
                break;
        }
    }
}

void HID_VendorRequest(void)
{
    /* Stall any unrecognised vendor request */
    HSUSBD_SET_CEP_STATE(HSUSBD_CEPCTL_STALLEN_Msk);
}
