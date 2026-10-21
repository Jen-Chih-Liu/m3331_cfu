/******************************************************************************
 * @file     cal_proto.c
 * @brief    Host-side command layer for the M3331 Calibration / OEM data-flash
 *           access channel (CFU application ISP report IDs 40/41).
 ******************************************************************************/
#include "cal_proto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "usb_hid.h"

/* Command-response timeouts (milliseconds). */
#define ACK_TIMEOUT_MS   1000
#define ERASE_TIMEOUT_MS 5000
#define SEND_TIMEOUT_MS  2000
#define RECV_POLL_MS     200

static uint32_t s_packno = 1;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Elapsed milliseconds since `start` (clock() ticks are milliseconds on MSVC). */
static long elapsed_ms(clock_t start)
{
    return (long)((clock() - start) * 1000 / CLOCKS_PER_SEC);
}

/*
 * Wait for the acknowledgement whose packet number equals s_packno + 1.
 * On success advances s_packno by 2 and copies the response into `resp`.
 */
static int wait_ack(uint8_t *resp, int timeout_ms)
{
    clock_t start = clock();

    while (elapsed_ms(start) < timeout_ms)
    {
        int n = hid_recv(resp, RECV_POLL_MS);

        if (n > 0)
        {
            if (get_u32(resp + 4) == s_packno + 1)
            {
                s_packno += 2;
                return CAL_OK;
            }
        }
        else if (n == HID_RECV_ERROR)
        {
            return CAL_ERR_IO;
        }
    }

    return CAL_ERR_TMOUT;
}

/*
 * Send a single-packet command and wait for the acknowledgement.
 *   addr/len are written at [8..11]/[12..15]; `data`/`data_len` fill [16..].
 * The response is returned in `resp`.
 */
static int cmd_packet(uint32_t cmd, uint32_t addr, uint32_t len,
                      const uint8_t *data, uint32_t data_len,
                      uint8_t *resp, int ack_timeout_ms)
{
    uint8_t req[HID_PACKET_SIZE];

    memset(req, 0, sizeof(req));
    put_u32(req + 0, cmd);
    put_u32(req + 4, s_packno);
    put_u32(req + 8, addr);
    put_u32(req + 12, len);

    if (data != NULL && data_len > 0)
    {
        if (data_len > (uint32_t)(HID_PACKET_SIZE - 16))
            data_len = HID_PACKET_SIZE - 16;
        memcpy(req + 16, data, (size_t)data_len);
    }

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return CAL_ERR_IO;

    return wait_ack(resp, ack_timeout_ms);
}

void cal_reset_packno(void)
{
    s_packno = 1;
}

int cal_connect(void)
{
    uint8_t resp[HID_PACKET_SIZE];
    return cmd_packet(CMD_CONNECT, 0, 0, NULL, 0, resp, ACK_TIMEOUT_MS);
}

int cal_sync_packno(void)
{
    uint8_t resp[HID_PACKET_SIZE];
    uint8_t req[HID_PACKET_SIZE];

    /* SYNC_PACKNO carries the desired packet number in the payload at [4..7]. */
    memset(req, 0, sizeof(req));
    put_u32(req + 0, CMD_SYNC_PACKNO);
    put_u32(req + 4, s_packno);

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return CAL_ERR_IO;

    return wait_ack(resp, ACK_TIMEOUT_MS);
}

int cal_get_deviceid(uint32_t *pdid)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_packet(CMD_GET_DEVICEID, 0, 0, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == CAL_OK && pdid != NULL)
        *pdid = get_u32(resp + 8);

    return r;
}

int cal_erase(uint32_t addr, uint32_t len)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r;

    if (len == 0)
        return CAL_ERR_ARG;

    r = cmd_packet(CMD_ERASE_DATA, addr, len, NULL, 0, resp, ERASE_TIMEOUT_MS);
    if (r != CAL_OK)
        return r;

    return (resp[8] == CAL_STS_OK) ? CAL_OK : CAL_ERR_STS;
}

int cal_write(uint32_t addr, const uint8_t *data, uint32_t len,
              cal_progress_cb progress)
{
    uint8_t resp[HID_PACKET_SIZE];
    uint32_t offset;
    int last_percent = -1;

    if (data == NULL || len == 0 || (len & 0x7u))
        return CAL_ERR_ARG;

    for (offset = 0; offset < len; offset += CAL_WRITE_CHUNK)
    {
        uint32_t chunk = len - offset;
        int r;

        if (chunk > CAL_WRITE_CHUNK)
            chunk = CAL_WRITE_CHUNK;

        r = cmd_packet(CMD_WRITE_DATA, addr + offset, chunk,
                       data + offset, chunk, resp, ACK_TIMEOUT_MS);
        if (r != CAL_OK)
            return r;
        if (resp[8] != CAL_STS_OK)
            return CAL_ERR_STS;

        if (progress != NULL)
        {
            int percent = (int)(((uint64_t)(offset + chunk) * 100) / len);
            if (percent != last_percent)
            {
                progress(percent);
                last_percent = percent;
            }
        }
    }

    if (progress != NULL && last_percent != 100)
        progress(100);

    return CAL_OK;
}

int cal_read(uint32_t addr, uint8_t *data, uint32_t len,
             cal_progress_cb progress)
{
    uint8_t resp[HID_PACKET_SIZE];
    uint32_t offset;
    int last_percent = -1;

    if (data == NULL || len == 0 || (len & 0x3u))
        return CAL_ERR_ARG;

    for (offset = 0; offset < len; offset += CAL_READ_CHUNK)
    {
        uint32_t chunk = len - offset;
        int r;

        if (chunk > CAL_READ_CHUNK)
            chunk = CAL_READ_CHUNK;

        r = cmd_packet(CMD_READ_DATA, addr + offset, chunk,
                       NULL, 0, resp, ACK_TIMEOUT_MS);
        if (r != CAL_OK)
            return r;

        /* Response data area begins at byte 8. */
        memcpy(data + offset, resp + 8, (size_t)chunk);

        if (progress != NULL)
        {
            int percent = (int)(((uint64_t)(offset + chunk) * 100) / len);
            if (percent != last_percent)
            {
                progress(percent);
                last_percent = percent;
            }
        }
    }

    if (progress != NULL && last_percent != 100)
        progress(100);

    return CAL_OK;
}
