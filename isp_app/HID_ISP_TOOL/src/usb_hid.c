/******************************************************************************
 * @file     usb_hid.c
 * @brief    Native Windows HID transport for the M3331 HID ISP bootloader.
 *
 * The bootloader enumerates as a standard vendor-defined HID device, so it is
 * accessed through the Windows HID class driver (SetupAPI + hid.dll) rather
 * than libusb. This avoids replacing the driver with WinUSB (Zadig).
 *
 * The firmware uses 64-byte reports without a report ID. Windows report
 * buffers include a leading report-ID byte (0x00), so the OS-level buffers are
 * one byte larger than HID_PACKET_SIZE.
 ******************************************************************************/
#include "usb_hid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

/* Upper bound for an OS report buffer (report ID + 64 data bytes, with slack). */
#define MAX_REPORT_LEN 96

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static USHORT g_in_len = HID_PACKET_SIZE + 1;
static USHORT g_out_len = HID_PACKET_SIZE + 1;

/* Report IDs prepended to OUT reports / expected on IN reports.
 * 0 = un-numbered report (LDROM bootloader); 40/41 = CFU application channel. */
static uint8_t g_out_report_id = 0x00;
static uint8_t g_in_report_id = 0x00;

void hid_set_report_ids(uint8_t out_id, uint8_t in_id)
{
    g_out_report_id = out_id;
    g_in_report_id = in_id;
}

int hid_open(uint16_t vid, uint16_t pid)
{
    GUID hid_guid;
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD index = 0;
    int found = 0;

    if (g_dev != INVALID_HANDLE_VALUE)
        return 0;

    HidD_GetHidGuid(&hid_guid);

    dev_info = SetupDiGetClassDevs(&hid_guid, NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "SetupDiGetClassDevs failed (%lu)\n", GetLastError());
        return -1;
    }

    iface.cbSize = sizeof(iface);

    while (!found &&
           SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, index, &iface))
    {
        DWORD required = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail;
        HANDLE h;

        index++;

        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, NULL, 0, &required, NULL);
        if (required == 0)
            continue;

        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(required);
        if (detail == NULL)
            continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (SetupDiGetDeviceInterfaceDetailA(dev_info, &iface, detail, required,
                                             NULL, NULL))
        {
            h = CreateFileA(detail->DevicePath,
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

            if (h != INVALID_HANDLE_VALUE)
            {
                HIDD_ATTRIBUTES attr;
                attr.Size = sizeof(attr);

                if (HidD_GetAttributes(h, &attr) &&
                    attr.VendorID == vid && attr.ProductID == pid)
                {
                    PHIDP_PREPARSED_DATA pp = NULL;

                    if (HidD_GetPreparsedData(h, &pp))
                    {
                        HIDP_CAPS caps;
                        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS)
                        {
                            g_in_len = caps.InputReportByteLength;
                            g_out_len = caps.OutputReportByteLength;
                        }
                        HidD_FreePreparsedData(pp);
                    }

                    if (g_in_len == 0 || g_in_len > MAX_REPORT_LEN)
                        g_in_len = HID_PACKET_SIZE + 1;
                    if (g_out_len == 0 || g_out_len > MAX_REPORT_LEN)
                        g_out_len = HID_PACKET_SIZE + 1;

                    g_dev = h;
                    found = 1;
                }
                else
                {
                    CloseHandle(h);
                }
            }
        }

        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    if (!found)
    {
        fprintf(stderr, "HID device not found (VID 0x%04x, PID 0x%04x)\n", vid, pid);
        return -1;
    }

    return 0;
}

void hid_close(void)
{
    if (g_dev != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_dev);
        g_dev = INVALID_HANDLE_VALUE;
    }
}

/* Perform an overlapped read/write with a timeout. Returns bytes transferred,
 * 0 on timeout, or -1 on error. */
static int overlapped_io(int is_read, uint8_t *buf, DWORD len, int timeout_ms)
{
    OVERLAPPED ov;
    DWORD transferred = 0;
    BOOL ok;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL)
        return -1;

    if (is_read)
        ok = ReadFile(g_dev, buf, len, &transferred, &ov);
    else
        ok = WriteFile(g_dev, buf, len, &transferred, &ov);

    if (!ok)
    {
        if (GetLastError() != ERROR_IO_PENDING)
        {
            CloseHandle(ov.hEvent);
            return -1;
        }

        if (WaitForSingleObject(ov.hEvent, (DWORD)timeout_ms) != WAIT_OBJECT_0)
        {
            CancelIo(g_dev);
            /* Reap the cancelled request so the buffer is no longer in use. */
            GetOverlappedResult(g_dev, &ov, &transferred, TRUE);
            CloseHandle(ov.hEvent);
            return 0; /* timeout */
        }
    }

    if (!GetOverlappedResult(g_dev, &ov, &transferred, TRUE))
    {
        CloseHandle(ov.hEvent);
        return -1;
    }

    CloseHandle(ov.hEvent);
    return (int)transferred;
}

int hid_send(const uint8_t *buf, int timeout_ms)
{
    uint8_t report[MAX_REPORT_LEN];
    int r;

    if (g_dev == INVALID_HANDLE_VALUE)
        return -1;

    memset(report, 0, sizeof(report));
    report[0] = g_out_report_id; /* report ID (0 = un-numbered) */
    memcpy(report + 1, buf, HID_PACKET_SIZE);

    r = overlapped_io(0, report, g_out_len, timeout_ms);
    if (r <= 0)
    {
        fprintf(stderr, "HID write failed\n");
        return -1;
    }

    return 0;
}

int hid_recv(uint8_t *buf, int timeout_ms)
{
    uint8_t report[MAX_REPORT_LEN];
    int r;

    if (g_dev == INVALID_HANDLE_VALUE)
        return HID_RECV_ERROR;

    memset(report, 0, sizeof(report));

    r = overlapped_io(1, report, g_in_len, timeout_ms);
    if (r == 0)
        return HID_RECV_TIMEOUT;
    if (r < 0)
        return HID_RECV_ERROR;

    /* On the CFU channel other input reports may exist; ignore mismatches so
     * the caller keeps polling until the expected response arrives. */
    if (g_in_report_id != 0 && report[0] != g_in_report_id)
        return HID_RECV_TIMEOUT;

    /* Strip the leading report-ID byte; return the 64 data bytes. */
    memcpy(buf, report + 1, HID_PACKET_SIZE);
    return HID_PACKET_SIZE;
}
