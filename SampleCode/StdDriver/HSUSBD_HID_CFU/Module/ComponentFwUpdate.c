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
    printf("Timer_Stop\n");
    TIMER_Stop(TIMER0);
}

void BSP_Timer_Restart(TIMER_ID timerId)
{
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
MCU_STATUS GetVersionImpl_comp02(UINT32* pVersion);
MCU_STATUS GetVersionImpl_comp30(UINT32* pVersion);
MCU_STATUS GetProductInfoImpl(UINT32* pProductInfo);
MCU_STATUS ProcessOfferImpl(FWUPDATE_OFFER_COMMAND* pCommand, FWUPDATE_OFFER_RESPONSE* pResponse);
MCU_STATUS GetCrcOffsetImpl(UINT32* pOffset);
MCU_STATUS NotifySuccessImpl(BOOL forceReset, READ_FIRMWARE_FUNC readHandler,
                             READ_COMPLETED_FUNC readCompleteHandler);

static ICOMPONENT_INTERFACE  s_IComp_Interface[64];
static uint8_t               fake_update = 0;

/* s_Comp_Registration_2 must be defined first so s_Comp_Registration can
 * take its address in the initialiser. */
volatile COMPONENT_REGISTRATION s_Comp_Registration_2 =
{
    .pNext = NULL,
    .interface = {
        .GetVersion     = GetVersionImpl_comp30,
        .GetProductInfo = GetProductInfoImpl,
        .ProcessOffer   = ProcessOfferImpl,
        .GetCrcOffset   = GetCrcOffsetImpl,
        .NotifySuccess  = NotifySuccessImpl
    },
    .componentId = COMPONENT_30
};

volatile COMPONENT_REGISTRATION s_Comp_Registration =
{
    .pNext = &s_Comp_Registration_2,
    .interface = {
        .GetVersion     = GetVersionImpl_comp02,
        .GetProductInfo = GetProductInfoImpl,
        .ProcessOffer   = ProcessOfferImpl,
        .GetCrcOffset   = GetCrcOffsetImpl,
        .NotifySuccess  = NotifySuccessImpl
    },
    .componentId = COMPONENT_02
};

static CURRENT_OFFER_INFO       s_currentOffer;
static COMPONENT_REGISTRATION  *s_pFirstComponentIFace = NULL;
static TIMER_ID                 s_updateTimer = 0;
static BOOL                     s_bankSwapPending = FALSE;

uint32_t g_u32Index = 0;
static uint32_t g_u32OfferStage   = 0;
static uint32_t g_u32ContentStage = 0;

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
        printf("TMR0_IRQ\n");
        TIMER_ClearIntFlag(TIMER0);
        _UpdateTimerCallback();
    }
}

/*---------------------------------------------------------------------------------------------------------*/
/* Component interface implementations                                                                     */
/*---------------------------------------------------------------------------------------------------------*/
MCU_STATUS GetVersionImpl_comp30(UINT32* pVersion)
{
    *pVersion = 0x7c000000;
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetVersionImpl_comp02(UINT32* pVersion)
{
    *pVersion = 0x7c000000;
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetProductInfoImpl(UINT32* pProductInfo)
{
    *pProductInfo = 0x87654321;
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS ProcessOfferImpl(FWUPDATE_OFFER_COMMAND* pCommand, FWUPDATE_OFFER_RESPONSE* pResponse)
{
    unsigned int target_version = 0x7B000000;
    printf("pCommand->version = %u\n", pCommand->version);

    if (pCommand->version > target_version)
    {
        memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
        pResponse->rejectReasonCode = FIRMWARE_OFFER_REJECT_OLD_FW;
        pResponse->token = pCommand->componentInfo.token;

        if (fake_update < 1)
        {
            pResponse->status = FIRMWARE_UPDATE_OFFER_ACCEPT;
            fake_update++;
        }
        else
        {
            pResponse->status = FIRMWARE_UPDATE_OFFER_REJECT;
        }
    }
    else
    {
        memset(pResponse, 0, sizeof(FWUPDATE_OFFER_RESPONSE));
        pResponse->rejectReasonCode = FIRMWARE_OFFER_REJECT_OLD_FW;
        pResponse->token = pCommand->componentInfo.token;
        pResponse->status = FIRMWARE_UPDATE_OFFER_REJECT;
    }

    return MCU_STATUS_SUCCESS;
}

MCU_STATUS GetCrcOffsetImpl(UINT32* pOffset)
{
    *pOffset = 0x0;
    return MCU_STATUS_SUCCESS;
}

MCU_STATUS NotifySuccessImpl(BOOL forceReset, READ_FIRMWARE_FUNC readHandler,
                             READ_COMPLETED_FUNC readCompleteHandler)
{
    readCompleteHandler();
    return MCU_STATUS_SUCCESS;
}

/*---------------------------------------------------------------------------------------------------------*/
/* FirmwareUpdateInit                                                                                      */
/*---------------------------------------------------------------------------------------------------------*/
UINT32 FirmwareUpdateInit(void)
{
    s_updateTimer = BSP_Timer_Create(_UpdateTimerCallback,
                                     MAX_FW_UPDATE_TIME_FAIL_SAFE_MS);
    printf("FirmwareUpdateInit\n");
    return 0;
}

/*---------------------------------------------------------------------------------------------------------*/
/* IComponentFirmwareUpdateRegisterComponent                                                               */
/*---------------------------------------------------------------------------------------------------------*/
void IComponentFirmwareUpdateRegisterComponent(COMPONENT_REGISTRATION* pRegistration)
{
    printf("IComponentFirmwareUpdateRegisterComponent\n");

    if (!pRegistration)
    {
        printf("!pRegistration\n");
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
    printf("ProcessCFWUContent\n");
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

    if (pCommand->flags & FIRMWARE_UPDATE_FLAG_LAST_BLOCK)
    {
        if (ICompFwUpdateBspWrite(pCommand->address, pCommand->pData,
                                  pCommand->length, componentId) == 0)
        {
            COMPONENT_REGISTRATION* pRegistration     = s_pFirstComponentIFace;
            MCU_STATUS              getCrcOffsetResult = MCU_STATUS_DEFAULT_ERROR;
            UINT32                  crcOffset          = 0;

            while (pRegistration)
            {
                if (pRegistration->componentId == componentId)
                {
                    getCrcOffsetResult = pRegistration->interface.GetCrcOffset(&crcOffset);
                    printf("getCrcOffsetResult = %d\n", getCrcOffsetResult);
                    break;
                }
                pRegistration = pRegistration->pNext;
            }

            if (!MCU_SUCCESS(getCrcOffsetResult))
            {
                status = FIRMWARE_UPDATE_STATUS_ERROR_INVALID;
            }
            else if (getCrcOffsetResult != MCU_STATUS_CFU_CRC_CHECK_NOT_REQUIRED)
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
            else
            {
                if (ICompFwUpdateBspAuthenticateFWImage() != 0)
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
        else
        {
            status = FIRMWARE_UPDATE_STATUS_ERROR_WRITE;
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
    printf("ProcessCFWUOffer\n");
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
            printf("OFFER_INFO_START_ENTIRE_TRANSACTION\n");
        else if (segmentNumber == OFFER_INFO_START_OFFER_LIST)
            printf("OFFER_INFO_START_OFFER_LIST\n");
        else if (segmentNumber == OFFER_INFO_END_OFFER_LIST)
            printf("OFFER_INFO_END_OFFER_LIST\n");

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
                    printf("ACCEPT (force)\n");
                }
            }

            if (pResponse->status == FIRMWARE_UPDATE_OFFER_ACCEPT)
            {
                BSP_Timer_Restart(s_updateTimer);
                s_currentOffer.updateInProgress = TRUE;
                s_currentOffer.forceReset       = forceReset;
                s_currentOffer.activeComponentId = componentId;
                printf("Offer ok\n");
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
    printf("ProcessCFWUGetFWVersion\n");
    memset(pResponse, 0, sizeof(GET_FWVERSION_RESPONSE));
    pResponse->header.fwUpdateRevision = CPFWU_REVISION;

    COMPONENT_REGISTRATION* pRegistration = s_pFirstComponentIFace;
    UINT8  componentCount = 0;
    UINT32* pVersion = (UINT32*)pResponse->versionAndProductInfoBlob;

    while (pRegistration)
    {
        if (pRegistration->interface.GetVersion == NULL)
        {
            printf("Error: GetVersion function pointer is NULL\n");
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
    printf("FwUpdate_Offer_Resp status = %d\n", s_FwUpdate_Offer_Resp.status);

    if (s_FwUpdate_Offer_Resp.status == FIRMWARE_UPDATE_OFFER_ACCEPT)
    {
        /* Content phase */
        printf("FIRMWARE_UPDATE_OFFER_ACCEPT ROUTE\n");
        memcpy((uint8_t *)&s_FwUpdate_Cont_Cmd[g_u32Index], pu8EpBuf, u32Size);
        g_u32Index += u32Size;

        if (u32Size >= 8 && g_u32ContentStage == 0)
        {
            ProcessCFWUContent(&s_FwUpdate_Cont_Cmd[g_u32Index - u32Size],
                               &s_FwUpdate_Cont_Resp);

            /* Prepare IN response: REPORT_ID_PAYLOAD_INPUT + 16 bytes response */
            g_u8CfuInBuf[0] = REPORT_ID_PAYLOAD_INPUT;
            memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Cont_Resp, 16);
            g_u32CfuInLen  = 17;
            g_u32ContentStage = 1;
            g_u8CfuInReady = 1;
        }
    }
    else
    {
        g_u32Index += u32Size;

        if ((g_u32Index == 16) && g_u32OfferStage < 8)
        {
            /* Offer phase */
            memcpy((uint8_t *)&s_FwUpdate_Offer_Cmd, pu8EpBuf, u32Size);
            ProcessCFWUOffer(&s_FwUpdate_Offer_Cmd, &s_FwUpdate_Offer_Resp);

            /* Prepare IN response: REPORT_ID_OFFER_INPUT + 16 bytes response */
            g_u8CfuInBuf[0] = REPORT_ID_OFFER_INPUT;
            memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Offer_Resp, 16);
            g_u32CfuInLen = 17;
            g_u32OfferStage++;
            g_u8CfuInReady = 1;
        }
        else
        {
            printf("NOT FIRMWARE_UPDATE_OFFER_ACCEPT ROUTE\n");
            memcpy((uint8_t *)&s_FwUpdate_Cont_Cmd[g_u32Index], pu8EpBuf, u32Size);
            g_u32Index += u32Size;

            if (u32Size >= 8 && g_u32ContentStage == 0)
            {
                ProcessCFWUContent(&s_FwUpdate_Cont_Cmd[g_u32Index - u32Size],
                                   &s_FwUpdate_Cont_Resp);

                g_u8CfuInBuf[0] = REPORT_ID_PAYLOAD_INPUT;
                memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Cont_Resp, 16);
                g_u32CfuInLen  = 17;
                g_u32ContentStage = 1;
                g_u8CfuInReady = 1;
            }
        }
    }
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
        printf("g_u32ContentStage IN\n");
        /* Prepare next response: REPORT_ID_PAYLOAD_OUTPUT + content response */
        g_u8CfuInBuf[0] = REPORT_ID_PAYLOAD_OUTPUT;
        memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Cont_Resp,
               sizeof(s_FwUpdate_Cont_Resp));
        g_u32CfuInLen = sizeof(s_FwUpdate_Cont_Resp) + 1;
        g_u32ContentStage = 0;
        g_u32Index = 0;
        g_u8CfuInReady = 0;

        /* Write to EPA FIFO */
        for (i = 0; i < g_u32CfuInLen; i++)
            HSUSBD->EP[EPA].EPDAT_BYTE = g_u8CfuInBuf[i];
        HSUSBD->EP[EPA].EPTXCNT = g_u32CfuInLen;
        return;
    }

    if (g_u32OfferStage == 1)
    {
        printf("g_u32OfferStage IN\n");
        /* Prepare next response: REPORT_ID_OFFER_OUTPUT + offer response */
        g_u8CfuInBuf[0] = REPORT_ID_OFFER_OUTPUT;
        memcpy(g_u8CfuInBuf + 1, (void *)&s_FwUpdate_Offer_Resp,
               sizeof(s_FwUpdate_Offer_Resp));
        g_u32CfuInLen = sizeof(s_FwUpdate_Offer_Resp) + 1;
        g_u32OfferStage = 0;
        g_u32Index = 0;
        g_u8CfuInReady = 0;

        /* Write to EPA FIFO */
        for (i = 0; i < g_u32CfuInLen; i++)
            HSUSBD->EP[EPA].EPDAT_BYTE = g_u8CfuInBuf[i];
        HSUSBD->EP[EPA].EPTXCNT = g_u32CfuInLen;
        return;
    }

    /* If g_u8CfuInReady is set but stages have not yet transitioned,
     * send the buffered data from CFU_GetOutReport */
    if (g_u8CfuInReady)
    {
        g_u8CfuInReady = 0;
        for (i = 0; i < g_u32CfuInLen; i++)
            HSUSBD->EP[EPA].EPDAT_BYTE = g_u8CfuInBuf[i];
        HSUSBD->EP[EPA].EPTXCNT = g_u32CfuInLen;
    }
}
