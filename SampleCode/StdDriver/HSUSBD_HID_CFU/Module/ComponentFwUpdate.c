/*++
    This file is part of Component Firmware Update (CFU), licensed under
    the MIT License (MIT).

    Copyright (c) Microsoft Corporation. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

Module Name:

    ComponentFwUpdate.c

Abstract:

    Implementation of the component firmware update protocol.
    Ported from M2351 (USBD) to m3331 (HSUSBD).

Environment:

    Firmware driver.
--*/
#include <stdlib.h>
#include <string.h>
#include "coretypes.h"
#include "ComponentFwUpdate.h"
#include "ICompFwUpdateBsp.h"
#include "IComponentFirmwareUpdate.h"
#include "FwVersion.h"
#include "NuMicro.h"
#include "hid_transfer.h"
#include "hid_cfu.h"

#define CPFWU_REVISION  (2u)

/* ACTIVE_BANK is set in the Keil target preprocessor:
 *   Target "AP0": ACTIVE_BANK=0  (firmware loaded at 0x00000, writes CFU to AP1)
 *   Target "AP1": ACTIVE_BANK=1  (firmware loaded at 0x20000, writes CFU to AP0)
 */
#ifndef ACTIVE_BANK
#define ACTIVE_BANK 0
#endif

#define ENTER_CRITICAL_SECTION() \
    uint32_t primask = __get_PRIMASK(); \
    __set_PRIMASK(1)

#define EXIT_CRITICAL_SECTION() \
    __set_PRIMASK(primask)

/*---------------------------------------------------------------------------------------------------------*/
/* Timer abstraction (m3331 TIMER0)                                                                        */
/*---------------------------------------------------------------------------------------------------------*/
typedef UINT16 TIMER_ID;

TIMER_ID BSP_Timer_Create(void (* pTimerCallback)(void), UINT32 timeoutMs)
{
    /* Configure TIMER0 in periodic mode.
     * HIRC = 12 MHz, prescaler = 249 -> tick = 250/12M ~= 20.83 us
     * Compare value = timeoutMs * 1000 / 20.83 ~= timeoutMs * 48 */
    TIMER0->CTL = TIMER_PERIODIC_MODE | (250 - 1);
    TIMER0->CMP = 48 * timeoutMs;
    TIMER_EnableInt(TIMER0);
    NVIC_EnableIRQ(TMR0_IRQn);
    TIMER_Start(TIMER0);
    return (TIMER_ID)1;
}

void BSP_Timer_Stop(TIMER_ID timerId)
{
    CFU_DBG("Timer_Stop\n");
    TIMER_Stop(TIMER0);
}

void BSP_Timer_Restart(TIMER_ID timerId)
{
    /* Stop → reset counter → clear any pending interrupt → restart.
     * TIMER_Start alone does NOT reset the counter; without this sequence
     * the timer fires immediately if it was nearly expired since boot. */
    TIMER_Stop(TIMER0);
    TIMER_ResetCounter(TIMER0);
    TIMER_ClearIntFlag(TIMER0);
    TIMER_Start(TIMER0);
}

/* Maximum time for a single image update to finish. */
#define MAX_FW_UPDATE_TIME_FAIL_SAFE_MS  (5 * 60 * 1000)  /* 5 minutes */

/*---------------------------------------------------------------------------------------------------------*/
/* Typedef                                                                                                 */
/*---------------------------------------------------------------------------------------------------------*/
typedef struct
{
    UINT8   activeComponentId;
    BOOL    forceReset;
    BOOL    updateInProgress;
} CURRENT_OFFER_INFO;

/*---------------------------------------------------------------------------------------------------------*/
/* Forward declarations                                                                                    */
/*---------------------------------------------------------------------------------------------------------*/
MCU_STATUS GetVersionImpl_comp30(UINT32* pVersion);
MCU_STATUS GetVersionImpl_comp31(UINT32* pVersion);
MCU_STATUS GetProductInfoImpl(UINT32* pProductInfo);
MCU_STATUS ProcessOfferImpl(FWUPDATE_OFFER_COMMAND* pCommand, FWUPDATE_OFFER_RESPONSE* pResponse);
MCU_STATUS GetCrcOffsetImpl(UINT32* pOffset);
MCU_STATUS NotifySuccessImpl(BOOL forceReset, READ_FIRMWARE_FUNC readHandler,
                             READ_COMPLETED_FUNC readCompleteHandler);

static ICOMPONENT_INTERFACE  s_IComp_Interface[64];

/* s_Comp_Registration_2 must be defined first so s_Comp_Registration can
 * take its address in the initialiser. */
volatile COMPONENT_REGISTRATION s_Comp_Registration_2 =
{
    .pNext = NULL,
    .interface = {
#if ACTIVE_BANK == 0
        .GetVersion     = GetVersionImpl_comp30,  /* AP0 running: report AP0 version */
#else
        .GetVersion     = GetVersionImpl_comp31,  /* AP1 running: report AP1 version */
#endif
        .GetProductInfo = GetProductInfoImpl,
        .ProcessOffer   = ProcessOfferImpl,
        .GetCrcOffset   = GetCrcOffsetImpl,
        .NotifySuccess  = NotifySuccessImpl
    },
#if ACTIVE_BANK == 0
    .componentId = COMPONENT_30   /* AP0 running */
#else
    .componentId = COMPONENT_31   /* AP1 running */
#endif
};

static CURRENT_OFFER_INFO       s_currentOffer;
static COMPONENT_REGISTRATION  *s_pFirstComponentIFace = NULL;
static TIMER_ID                 s_updateTimer = 0;
static BOOL                     s_bankSwapPending = FALSE;

uint32_t g_u32Index = 0;
static uint32_t g_u32OfferStage   = 0;
static uint32_t g_u32ContentStage = 0;
volatile uint8_t g_u8ResetPending = 0;  /* set after bank switch; main loop reboots after final IN report sent */

static FWUPDATE_OFFER_COMMAND         s_FwUpdate_Offer_Cmd;
FWUPDATE_OFFER_RESPONSE               s_FwUpdate_Offer_Resp;
static FWUPDATE_CONTENT_COMMAND       s_FwUpdate_Cont_Cmd[8 + MAX_UINT8];
static FWUPDATE_CONTENT_RESPONSE      s_FwUpdate_Cont_Resp;

/*---------------------------------------------------------------------------------------------------------*/
/* Static helpers                                                                                          */
/*---------------------------------------------------------------------------------------------------------*/
static void _ReadCompleteCallback(void)
{
    s_currentOffer.updateInProgress = FALSE;
}

static void _UpdateTimerCallback(void)
{
    ENTER_CRITICAL_SECTION();
    s_currentOffer.updateInProgress = FALSE;
    BSP_Timer_Stop(s_updateTimer);
    EXIT_CRITICAL_SECTION();
}

void TMR0_IRQHandler(void)
{
    if (TIMER_GetIntFlag(TIMER0) == 1)
    {
        CFU_DBG("TMR0_IRQ\n");
        TIMER_ClearIntFlag(TIMER0);
        _UpdateTimerCallback();
    }
}

/*---------------------------------------------------------------------------------------------------------*/
/* Firmware version — placed at ROM_SIZE - 16 bytes via scatter file section .fw_version.                 */
/*   AP0: flash 0x1FFF0, binary offset 0x1FFF0                                                             */
/*   AP1: flash 0x3FFF0, binary offset 0x1FFF0 (objcopy strips base)                                      */
/* Flash tail layout:                                                                                      */
/*   0x1FFF0 [4B] FW_VERSION  (this variable)                                                              */
/*   0x1FFF4 [4B] (unused / 0xFF)                                                                          */
/*   0x1FFF8 [4B] FW_CHECKSUM (written post-build by gen_checksum_bin.py)                                  */
/*   0x1FFFC [4B] (unused / 0xFF)                                                                          */
/* gen_offer_bin.py: --fw-bin <file> --version-offset 0x1FFF0                                              */
/*---------------------------------------------------------------------------------------------------------*/
#define FW_VERSION_VALUE   0x03000002u

const uint32_t g_FwVersion __attribute__((section(".fw_version"), used)) = FW_VERSION_VALUE;

/*---------------------------------------------------------------------------------------------------------*/
/* Component interface implementations                                                                     */
/*---------------------------------------------------------------------------------------------------------*/
MCU_STATUS GetVersionImpl_comp30(UINT32* pVersion)
{
    *pVersion = g_FwVersion;   /* AP0 firmware version — defined above */
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetVersionImpl_comp31(UINT32* pVersion)
{
    *pVersion = g_FwVersion;   /* AP1 firmware version — defined above */
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetProductInfoImpl(UINT32* pProductInfo)
{
    *pProductInfo = 0x87654321;
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS ProcessOfferImpl(FWUPDATE_OFFER_COMMAND* pCommand, FWUPDATE_OFFER_RESPONSE* pResponse)
{
    /* Accept if offered version is strictly newer than the running firmware. */
    CFU_DBG("pCommand->version = 0x%08X, g_FwVersion = 0x%08X\n",
           pCommand->version, g_FwVersion);

    memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
    pResponse->token = pCommand->componentInfo.token;

    if (pCommand->version > g_FwVersion)
    {
        pResponse->status = FIRMWARE_UPDATE_OFFER_ACCEPT;
    }
    else
    {
        pResponse->rejectReasonCode = FIRMWARE_OFFER_REJECT_OLD_FW;
        pResponse->status           = FIRMWARE_UPDATE_OFFER_REJECT;
    }

    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetCrcOffsetImpl(UINT32* pOffset)
{
    /* The firmware binary does not embed a CRC at a fixed offset.
     * Signal the protocol layer to skip the read-back CRC compare and
     * proceed directly to ICompFwUpdateBspAuthenticateFWImage(). */
    (void)pOffset;
    return MCU_STATUS_CFU_CRC_CHECK_NOT_REQUIRED;
}

MCU_STATUS NotifySuccessImpl(BOOL forceReset, READ_FIRMWARE_FUNC readHandler,
                             READ_COMPLETED_FUNC readCompleteHandler)
{
    (void)readHandler;
    (void)forceReset;

    CFU_DBG("NotifySuccessImpl: firmware update complete, switching boot source\n");

    /* Mark update as finished */
    readCompleteHandler();

    /* Switch boot source to the bank we just programmed (the inactive one) */
    SYS_UnlockReg();
    FMC_Open();
#if ACTIVE_BANK == 0
     /* Running AP0 → boot AP1 on next reset */
	
	 FMC_SetVectorPageAddr(0x20000);
	
#else
	 /* Running AP1 → boot AP0 on next reset */
	 FMC_SetVectorPageAddr(0x00000);
     
#endif
    FMC_Close();

    /* Defer reset until the final content SUCCESS report has been sent to
     * the host (done in CFU_SetInReport). Resetting here would drop the
     * USB device before the host reads the response, causing the host tool
     * to report "device not functioning". */
    g_u8ResetPending = 1;
    return MCU_STATUS_SUCCESS;
}

/*---------------------------------------------------------------------------------------------------------*/
/* FirmwareUpdateInit                                                                                      */
/*---------------------------------------------------------------------------------------------------------*/
UINT32 FirmwareUpdateInit(void)
{
    s_updateTimer = BSP_Timer_Create(_UpdateTimerCallback,
                                     MAX_FW_UPDATE_TIME_FAIL_SAFE_MS);
    CFU_DBG("FirmwareUpdateInit\n");
    return 0;
}

/*---------------------------------------------------------------------------------------------------------*/
/* IComponentFirmwareUpdateRegisterComponent                                                               */
/*---------------------------------------------------------------------------------------------------------*/
void IComponentFirmwareUpdateRegisterComponent(COMPONENT_REGISTRATION* pRegistration)
{
    CFU_DBG("IComponentFirmwareUpdateRegisterComponent\n");

    if (!pRegistration)
    {
        CFU_DBG("!pRegistration\n");
        return;
    }

    ENTER_CRITICAL_SECTION();
    {
        pRegistration->pNext = s_pFirstComponentIFace;
        s_pFirstComponentIFace = pRegistration;
    }
    EXIT_CRITICAL_SECTION();
}

/*---------------------------------------------------------------------------------------------------------*/
/* ProcessCFWUContent                                                                                      */
/*---------------------------------------------------------------------------------------------------------*/
void ProcessCFWUContent(FWUPDATE_CONTENT_COMMAND* pCommand,
                        FWUPDATE_CONTENT_RESPONSE* pResponse)
{
    CFU_DBG("ProcessCFWUContent\n");
    UINT8  status         = FIRMWARE_UPDATE_STATUS_SUCCESS;
    UINT8  length         = pCommand->length;
    UINT16 sequenceNumber = pCommand->sequenceNumber;
    UINT8  componentId    = s_currentOffer.activeComponentId;

    if (pCommand->flags & FIRMWARE_UPDATE_FLAG_FIRST_BLOCK)
    {
        if (ICompFwUpdateBspPrepare(componentId) == 0)
        {
            if (ICompFwUpdateBspWrite(pCommand->address, pCommand->pData,
                                      pCommand->length, componentId) != 0)
                status = FIRMWARE_UPDATE_STATUS_ERROR_WRITE;
        }
        else
        {
            status = FIRMWARE_UPDATE_STATUS_ERROR_PREPARE;
        }
    }

    if ((pCommand->flags & FIRMWARE_UPDATE_FLAG_LAST_BLOCK) &&
        status == FIRMWARE_UPDATE_STATUS_SUCCESS)
    {
        /* Write only if this block was NOT also the first block.
         * First-block data was already written in the handler above. */
        if (!(pCommand->flags & FIRMWARE_UPDATE_FLAG_FIRST_BLOCK))
        {
            if (ICompFwUpdateBspWrite(pCommand->address, pCommand->pData,
                                      pCommand->length, componentId) != 0)
                status = FIRMWARE_UPDATE_STATUS_ERROR_WRITE;
        }

        if (status == FIRMWARE_UPDATE_STATUS_SUCCESS)
        {
            COMPONENT_REGISTRATION* pRegistration     = s_pFirstComponentIFace;
            MCU_STATUS              getCrcOffsetResult = MCU_STATUS_DEFAULT_ERROR;
            UINT32                  crcOffset          = 0;

            while (pRegistration)
            {
                if (pRegistration->componentId == componentId)
                {
                    getCrcOffsetResult = pRegistration->interface.GetCrcOffset(&crcOffset);
                    CFU_DBG("getCrcOffsetResult = %d\n", getCrcOffsetResult);
                    break;
                }
                pRegistration = pRegistration->pNext;
            }

            if (getCrcOffsetResult == MCU_STATUS_CFU_CRC_CHECK_NOT_REQUIRED)
            {
                /* CRC compare skipped: authenticate the written image directly. */
                if (ICompFwUpdateBspAuthenticateFWImage() != 0)
                    status = FIRMWARE_UPDATE_STATUS_ERROR_SIGNATURE;
            }
            else if (!MCU_SUCCESS(getCrcOffsetResult))
            {
                status = FIRMWARE_UPDATE_STATUS_ERROR_INVALID;
            }
            else
            {
                UINT16 crc;
                UINT16 calculatedCrc;

                if (ICompFwUpdateBspCalcCRC(&calculatedCrc, componentId) != 0)
                    status = FIRMWARE_UPDATE_STATUS_ERROR_CRC;
                else if (ICompFwUpdateBspRead(crcOffset, (UINT8*)&crc, 4, componentId) != 0)
                    status = FIRMWARE_UPDATE_STATUS_ERROR_CRC;
                else if (crc != calculatedCrc)
                    status = FIRMWARE_UPDATE_STATUS_ERROR_CRC;
                else if (ICompFwUpdateBspAuthenticateFWImage() != 0)
                    status = FIRMWARE_UPDATE_STATUS_ERROR_SIGNATURE;
            }

            if (status == FIRMWARE_UPDATE_STATUS_SUCCESS)
            {
                if (MCU_SUCCESS(pRegistration->interface.NotifySuccess(
                                    s_currentOffer.forceReset,
                                    ICompFwUpdateBspRead,
                                    _ReadCompleteCallback)))
                    s_bankSwapPending = TRUE;
                else
                    status = FIRMWARE_UPDATE_STATUS_ERROR_COMPLETE;
            }
        }
    }

    if (!(pCommand->flags & FIRMWARE_UPDATE_FLAG_LAST_BLOCK) &&
        !(pCommand->flags & FIRMWARE_UPDATE_FLAG_FIRST_BLOCK))
    {
        if (ICompFwUpdateBspWrite(pCommand->address, pCommand->pData,
                                  pCommand->length, componentId) != 0)
            status = FIRMWARE_UPDATE_STATUS_ERROR_WRITE;
    }

    if (status != FIRMWARE_UPDATE_STATUS_SUCCESS)
        s_currentOffer.updateInProgress = FALSE;

    if ((pCommand->flags & FIRMWARE_UPDATE_FLAG_LAST_BLOCK) &&
        status == FIRMWARE_UPDATE_STATUS_SUCCESS)
        s_FwUpdate_Offer_Resp.status = 0;

    memset(pResponse, 0, sizeof(FWUPDATE_CONTENT_RESPONSE));
    pResponse->sequenceNumber = sequenceNumber;
    pResponse->status = status;
}

/*---------------------------------------------------------------------------------------------------------*/
/* ProcessCFWUOffer                                                                                        */
/*---------------------------------------------------------------------------------------------------------*/
void ProcessCFWUOffer(FWUPDATE_OFFER_COMMAND* pCommand,
                      FWUPDATE_OFFER_RESPONSE* pResponse)
{
    CFU_DBG("ProcessCFWUOffer\n");
    UINT8 token       = pCommand->componentInfo.token;
    UINT8 componentId = pCommand->componentInfo.componentId;
    UINT8 segmentNumber = pCommand->componentInfo.segmentNumber;
    pResponse->rejectReasonCode = 0;
    pResponse->token = token;

    if (componentId == 0xFF)   /* Information Command */
    {
        pResponse->token  = token;
        pResponse->status = 0;
        pResponse->rejectReasonCode = 0;

        if (segmentNumber == OFFER_INFO_START_ENTIRE_TRANSACTION)
        {
            CFU_DBG("OFFER_INFO_START_ENTIRE_TRANSACTION\n");
            /* Reset all transaction state so a retry or second update works correctly */
            memset(&s_FwUpdate_Offer_Resp, 0, sizeof(s_FwUpdate_Offer_Resp));
            s_currentOffer.updateInProgress = FALSE;
            s_currentOffer.forceReset       = FALSE;
            s_bankSwapPending               = FALSE;
            g_u32Index                      = 0;
            g_u32OfferStage                 = 0;
            g_u32ContentStage               = 0;
        }
        else if (segmentNumber == OFFER_INFO_START_OFFER_LIST)
            CFU_DBG("OFFER_INFO_START_OFFER_LIST\n");
        else if (segmentNumber == OFFER_INFO_END_OFFER_LIST)
            CFU_DBG("OFFER_INFO_END_OFFER_LIST\n");

        return;
    }

    if (s_currentOffer.updateInProgress)
    {
        memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
        pResponse->status           = FIRMWARE_UPDATE_OFFER_BUSY;
        pResponse->rejectReasonCode = FIRMWARE_UPDATE_OFFER_BUSY;
        pResponse->token            = token;
        return;
    }
    else if (componentId == CFU_SPECIAL_OFFER_CMD)
    {
        FWUPDATE_SPECIAL_OFFER_COMMAND* pSpecialCommand =
            (FWUPDATE_SPECIAL_OFFER_COMMAND*)pCommand;

        if (pSpecialCommand->componentInfo.commandCode == CFU_SPECIAL_OFFER_GET_STATUS)
        {
            memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
            pResponse->status = FIRMWARE_UPDATE_OFFER_COMMAND_READY;
            pResponse->token  = token;
            return;
        }
    }
    else if (s_bankSwapPending)
    {
        memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
        pResponse->status           = FIRMWARE_UPDATE_OFFER_REJECT;
        pResponse->rejectReasonCode = FIRMWARE_UPDATE_OFFER_SWAP_PENDING;
        pResponse->token            = token;
        return;
    }

    COMPONENT_REGISTRATION* pRegistration = s_pFirstComponentIFace;

    while (pRegistration)
    {
        if (pRegistration->componentId == componentId)
        {
            BOOL forceReset    = pCommand->componentInfo.forceImmediateReset;
            BOOL ignoreVersion = pCommand->componentInfo.forceIgnoreVersion;

            pRegistration->interface.ProcessOffer(pCommand, pResponse);

            if (ignoreVersion)
            {
                if ((pResponse->status == FIRMWARE_UPDATE_OFFER_REJECT)
                 && (pResponse->rejectReasonCode == FIRMWARE_OFFER_REJECT_OLD_FW))
                {
                    pResponse->status = FIRMWARE_UPDATE_OFFER_ACCEPT;
                    CFU_DBG("ACCEPT (force)\n");
                }
            }

            if (pResponse->status == FIRMWARE_UPDATE_OFFER_ACCEPT)
            {
                BSP_Timer_Restart(s_updateTimer);
                s_currentOffer.updateInProgress = TRUE;
                s_currentOffer.forceReset       = forceReset;
                s_currentOffer.activeComponentId = componentId;
                CFU_DBG("Offer ok\n");
            }
        }

        pRegistration = pRegistration->pNext;
    }
}

/*---------------------------------------------------------------------------------------------------------*/
/* ProcessCFWUGetFWVersion                                                                                 */
/*---------------------------------------------------------------------------------------------------------*/
void ProcessCFWUGetFWVersion(GET_FWVERSION_RESPONSE* pResponse)
{
    CFU_DBG("ProcessCFWUGetFWVersion\n");
    memset(pResponse, 0, sizeof(GET_FWVERSION_RESPONSE));
    pResponse->header.fwUpdateRevision = CPFWU_REVISION;

    COMPONENT_REGISTRATION* pRegistration = s_pFirstComponentIFace;
    UINT8  componentCount = 0;
    UINT32* pVersion = (UINT32*)pResponse->versionAndProductInfoBlob;

    while (pRegistration)
    {
        if (pRegistration->interface.GetVersion == NULL)
        {
            CFU_DBG("Error: GetVersion function pointer is NULL\n");
            break;
        }
        pRegistration->interface.GetVersion(pVersion);
        pVersion++;
        pRegistration->interface.GetProductInfo(pVersion);
        pVersion++;
        pRegistration = pRegistration->pNext;
        componentCount++;
    }

    pResponse->header.componentCount = componentCount;
}

/*---------------------------------------------------------------------------------------------------------*/
/* CFU_GetOutReport - called from EPB_Handler when OUT data arrives                                        */
/*   pu8EpBuf : pointer to received data (Report ID already stripped by EPB_Handler)                      */
/*   u32Size  : number of bytes in pu8EpBuf                                                               */
/*---------------------------------------------------------------------------------------------------------*/
void CFU_GetOutReport(uint8_t *pu8EpBuf, uint32_t u32Size)
{
    CFU_DBG("CFU_GetOutReport: offer_status=%d size=%u\n",
           s_FwUpdate_Offer_Resp.status, u32Size);

    if (s_FwUpdate_Offer_Resp.status == FIRMWARE_UPDATE_OFFER_ACCEPT)
    {
        /* Content phase: each OUT packet is one complete FWUPDATE_CONTENT_COMMAND.
         * Process every block immediately — no accumulation, no stage gating. */
        CFU_DBG("Content block received\n");
        memcpy((uint8_t *)&s_FwUpdate_Cont_Cmd[0], pu8EpBuf,
               u32Size < sizeof(s_FwUpdate_Cont_Cmd[0]) ? u32Size : sizeof(s_FwUpdate_Cont_Cmd[0]));

        ProcessCFWUContent((FWUPDATE_CONTENT_COMMAND *)&s_FwUpdate_Cont_Cmd[0],
                           &s_FwUpdate_Cont_Resp);

        /* Write response to EPA FIFO immediately */
        g_u32ContentStage = 1;
        g_u8CfuInReady    = 1;
    }
    else
    {
        /* Offer phase: each OUT packet is one complete FWUPDATE_OFFER_COMMAND. */
        CFU_DBG("Offer packet received\n");
        memcpy((uint8_t *)&s_FwUpdate_Offer_Cmd, pu8EpBuf,
               u32Size < sizeof(s_FwUpdate_Offer_Cmd) ? u32Size : sizeof(s_FwUpdate_Offer_Cmd));

        ProcessCFWUOffer(&s_FwUpdate_Offer_Cmd, &s_FwUpdate_Offer_Resp);

        /* Write response to EPA FIFO immediately */
        g_u32OfferStage = 1;
        g_u8CfuInReady  = 1;
    }

    /* Write the queued response to EPA FIFO and arm INTKIEN so the
     * host's ReadFile (Interrupt IN) receives the report. */
    CFU_SetInReport();
}

/*---------------------------------------------------------------------------------------------------------*/
/* CFU_SetInReport - called from EPA_Handler when host polls IN endpoint                                   */
/*   Writes queued response data into HSUSBD EPA FIFO.                                                     */
/*---------------------------------------------------------------------------------------------------------*/
void CFU_SetInReport(void)
{
    uint32_t i;

    if (g_u32ContentStage == 1)
    {
        CFU_DBG("g_u32ContentStage IN\n");
        /* Prepare response: REPORT_ID_PAYLOAD_INPUT + content response */
        g_u8CfuInBuf[0] = REPORT_ID_PAYLOAD_INPUT;
        memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Cont_Resp,
               sizeof(s_FwUpdate_Cont_Resp));
        g_u32CfuInLen = sizeof(s_FwUpdate_Cont_Resp) + 1;
        g_u32ContentStage = 0;
        g_u32Index = 0;
        g_u8CfuInReady = 0;

        /* Write to EPA FIFO and arm interrupt so host's ReadFile receives it */
        for (i = 0; i < g_u32CfuInLen; i++)
            HSUSBD->EP[EPA].EPDAT_BYTE = g_u8CfuInBuf[i];
        HSUSBD->EP[EPA].EPTXCNT = g_u32CfuInLen;
        HSUSBD_ENABLE_EP_INT(EPA, HSUSBD_EPINTEN_INTKIEN_Msk);

        /* If the final block completed the update, the host has now been
         * given the SUCCESS response. The actual reboot is deferred to the
         * main loop (g_u8ResetPending) so the USB control transfer and IN
         * read can finish first; resetting here drops USB mid-transfer and
         * the host tool reports "device not functioning". */
        return;
    }

    if (g_u32OfferStage == 1)
    {
        CFU_DBG("g_u32OfferStage IN\n");
        /* Prepare response: REPORT_ID_OFFER_INPUT + offer response */
        g_u8CfuInBuf[0] = REPORT_ID_OFFER_INPUT;
        memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Offer_Resp,
               sizeof(s_FwUpdate_Offer_Resp));
        g_u32CfuInLen = sizeof(s_FwUpdate_Offer_Resp) + 1;
        g_u32OfferStage = 0;
        g_u32Index = 0;
        g_u8CfuInReady = 0;

        /* Write to EPA FIFO and arm interrupt so host's ReadFile receives it */
        for (i = 0; i < g_u32CfuInLen; i++)
            HSUSBD->EP[EPA].EPDAT_BYTE = g_u8CfuInBuf[i];
        HSUSBD->EP[EPA].EPTXCNT = g_u32CfuInLen;
        HSUSBD_ENABLE_EP_INT(EPA, HSUSBD_EPINTEN_INTKIEN_Msk);
        return;
    }

    /* No pending data: disable INTKIEN to stop spurious EPA interrupts */
    HSUSBD_ENABLE_EP_INT(EPA, 0);
}
