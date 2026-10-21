/******************************************************************************
 * @file     main.c
 * @brief    m3331_cal_tool - host utility to erase/read/write the Calibration
 *           and OEM-config data-flash regions of the Nuvoton M3331 while the
 *           application firmware (HSUSBD_HID_CFU) runs in APROM.
 *
 * Flash layout (device):
 *   Bank0  0x00000..0x1EFFF  (124 KB firmware)
 *   Bank1  0x1F000..0x3DFFF  (124 KB firmware)
 *   Calib  0x3E000..0x3EFFF  (4 KB)   <- writable here
 *   OEM    0x3F000..0x3FFFF  (4 KB)   <- writable here
 *
 * Usage:
 *   m3331_cal_tool --info               Show device ID / channel
 *   m3331_cal_tool --write-calib <bin>  Erase+program+verify Calibration (4 KB)
 *   m3331_cal_tool --write-oem   <bin>  Erase+program+verify OEM config (4 KB)
 *   m3331_cal_tool --read-calib  <bin>  Read 4 KB Calibration into <bin>
 *   m3331_cal_tool --read-oem    <bin>  Read 4 KB OEM config into <bin>
 *   m3331_cal_tool --help               Show this help
 *
 * The device must be running the CFU application (VID 0x0416 / PID 0xF502,
 * HID report IDs 40 / 41).
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal_proto.h"
#include "usb_hid.h"

static void print_usage(const char *prog)
{
    printf("m3331_cal_tool - Calibration / OEM data-flash utility for Nuvoton M3331\n\n");
    printf("Usage:\n");
    printf("  %s --info               Show device info (no flash access)\n", prog);
    printf("  %s --write-calib <bin>  Erase+program+verify Calibration (0x3E000, 4 KB)\n", prog);
    printf("  %s --write-oem   <bin>  Erase+program+verify OEM config  (0x3F000, 4 KB)\n", prog);
    printf("  %s --read-calib  <bin>  Read 4 KB Calibration into <bin>\n", prog);
    printf("  %s --read-oem    <bin>  Read 4 KB OEM config  into <bin>\n", prog);
    printf("  %s --help               Show this help\n", prog);
    printf("\nDevice must be in the CFU application (VID 0x0416 / PID 0xF502).\n");
}

static void progress_cb(int percent)
{
    printf("\r  progress: %3d%%", percent);
    fflush(stdout);
}

static const char *cal_strerror(int rc)
{
    switch (rc)
    {
    case CAL_OK:        return "OK";
    case CAL_ERR_IO:    return "USB I/O error";
    case CAL_ERR_TMOUT: return "timeout waiting for device";
    case CAL_ERR_STS:   return "device rejected request (range/align/flash error)";
    case CAL_ERR_ARG:   return "invalid argument";
    default:            return "unknown error";
    }
}

/* Load a binary file into a freshly allocated buffer. Caller frees *out_buf. */
static int load_bin(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
    FILE *fp;
    long size;
    uint8_t *buf;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        fprintf(stderr, "Cannot open file: %s\n", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0)
    {
        fprintf(stderr, "File is empty: %s\n", path);
        fclose(fp);
        return -1;
    }

    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        fclose(fp);
        return -1;
    }

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        fprintf(stderr, "Failed to read file: %s\n", path);
        free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_size = (uint32_t)size;
    return 0;
}

/* Write `size` bytes from `buf` to `path`. Returns 0 on success. */
static int save_bin(const char *path, const uint8_t *buf, uint32_t size)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL)
    {
        fprintf(stderr, "Cannot create file: %s\n", path);
        return -1;
    }

    if (fwrite(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        fprintf(stderr, "Failed to write file: %s\n", path);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/* Open the CFU device and establish an ISP session. */
static int open_and_connect(void)
{
    uint32_t pdid = 0;

    hid_set_report_ids(CAL_REPORT_ID_OUT, CAL_REPORT_ID_IN);
    if (hid_open(CAL_USB_VID, CAL_USB_PID) != 0)
    {
        fprintf(stderr,
                "Cannot open device (VID 0x%04X / PID 0x%04X).\n"
                "Make sure the CFU application firmware is running.\n",
                CAL_USB_VID, CAL_USB_PID);
        return -1;
    }

    cal_reset_packno();

    if (cal_connect() != CAL_OK)
    {
        fprintf(stderr, "CONNECT failed\n");
        return -1;
    }
    if (cal_sync_packno() != CAL_OK)
    {
        fprintf(stderr, "SYNC_PACKNO failed\n");
        return -1;
    }

    printf("Channel   : CFU application (PID 0x%04X, report %d/%d)\n",
           CAL_USB_PID, CAL_REPORT_ID_OUT, CAL_REPORT_ID_IN);
    if (cal_get_deviceid(&pdid) == CAL_OK)
        printf("Device ID : 0x%08X\n", pdid);

    return 0;
}

/* Erase + program + read-back verify a 4 KB region. Returns 0 on success. */
static int do_write_region(uint32_t base, const char *region, const char *path)
{
    uint8_t *image = NULL;
    uint32_t image_size = 0;
    uint8_t *padded = NULL;
    uint8_t *readback = NULL;
    int rc = 1;
    int r;

    if (load_bin(path, &image, &image_size) != 0)
        return 1;

    if (image_size > REGION_SIZE)
    {
        fprintf(stderr, "File too large for %s region (%u bytes, max %u)\n",
                region, image_size, (unsigned)REGION_SIZE);
        goto cleanup;
    }

    /* Pad up to an 8-byte boundary with 0xFF (erased value). The device
     * programs flash 64 bits at a time, so writes must be 8-byte aligned. */
    uint32_t prog_size = (image_size + 7u) & ~7u;
    padded = (uint8_t *)malloc(prog_size);
    readback = (uint8_t *)malloc(prog_size);
    if (padded == NULL || readback == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        goto cleanup;
    }
    memset(padded, 0xFF, prog_size);
    memcpy(padded, image, image_size);

    printf("Region    : %s (0x%05X, %u bytes from file)\n",
           region, (unsigned)base, image_size);

    printf("Erasing   : 0x%05X..0x%05X\n",
           (unsigned)base, (unsigned)(base + REGION_SIZE - 1));
    r = cal_erase(base, REGION_SIZE);
    if (r != CAL_OK)
    {
        fprintf(stderr, "Erase failed: %s\n", cal_strerror(r));
        goto cleanup;
    }

    printf("Writing   : %u bytes\n", prog_size);
    r = cal_write(base, padded, prog_size, progress_cb);
    printf("\n");
    if (r != CAL_OK)
    {
        fprintf(stderr, "Write failed: %s\n", cal_strerror(r));
        goto cleanup;
    }

    printf("Verifying : reading back %u bytes\n", prog_size);
    r = cal_read(base, readback, prog_size, progress_cb);
    printf("\n");
    if (r != CAL_OK)
    {
        fprintf(stderr, "Read-back failed: %s\n", cal_strerror(r));
        goto cleanup;
    }

    if (memcmp(padded, readback, prog_size) != 0)
    {
        fprintf(stderr, "VERIFY FAILED: read-back does not match written data\n");
        goto cleanup;
    }

    printf("Result    : SUCCESS (%s verified)\n", region);
    rc = 0;

cleanup:
    free(image);
    free(padded);
    free(readback);
    return rc;
}

/* Read a 4 KB region into a file. Returns 0 on success. */
static int do_read_region(uint32_t base, const char *region, const char *path)
{
    uint8_t *buf = (uint8_t *)malloc(REGION_SIZE);
    int rc = 1;
    int r;

    if (buf == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    printf("Region    : %s (0x%05X, %u bytes)\n",
           region, (unsigned)base, (unsigned)REGION_SIZE);
    printf("Reading   : into %s\n", path);

    r = cal_read(base, buf, REGION_SIZE, progress_cb);
    printf("\n");
    if (r != CAL_OK)
    {
        fprintf(stderr, "Read failed: %s\n", cal_strerror(r));
        goto cleanup;
    }

    if (save_bin(path, buf, REGION_SIZE) != 0)
        goto cleanup;

    printf("Result    : SUCCESS (%u bytes saved to %s)\n",
           (unsigned)REGION_SIZE, path);
    rc = 0;

cleanup:
    free(buf);
    return rc;
}

/* Supported operations. */
enum { OP_NONE, OP_INFO, OP_WRITE_CALIB, OP_WRITE_OEM, OP_READ_CALIB, OP_READ_OEM };

int main(int argc, char *argv[])
{
    int op = OP_NONE;
    const char *path = NULL;
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--info") == 0)
        {
            op = OP_INFO;
        }
        else if (strcmp(argv[i], "--write-calib") == 0)
        {
            op = OP_WRITE_CALIB;
            if (i + 1 >= argc) { fprintf(stderr, "--write-calib requires a file\n"); return 1; }
            path = argv[++i];
        }
        else if (strcmp(argv[i], "--write-oem") == 0)
        {
            op = OP_WRITE_OEM;
            if (i + 1 >= argc) { fprintf(stderr, "--write-oem requires a file\n"); return 1; }
            path = argv[++i];
        }
        else if (strcmp(argv[i], "--read-calib") == 0)
        {
            op = OP_READ_CALIB;
            if (i + 1 >= argc) { fprintf(stderr, "--read-calib requires a file\n"); return 1; }
            path = argv[++i];
        }
        else if (strcmp(argv[i], "--read-oem") == 0)
        {
            op = OP_READ_OEM;
            if (i + 1 >= argc) { fprintf(stderr, "--read-oem requires a file\n"); return 1; }
            path = argv[++i];
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (op == OP_NONE)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (open_and_connect() != 0)
        return 1;

    switch (op)
    {
    case OP_INFO:
        /* Info already printed by open_and_connect(). */
        break;
    case OP_WRITE_CALIB:
        rc = do_write_region(CALIB_BASE, "Calibration", path);
        break;
    case OP_WRITE_OEM:
        rc = do_write_region(OEM_BASE, "OEM config", path);
        break;
    case OP_READ_CALIB:
        rc = do_read_region(CALIB_BASE, "Calibration", path);
        break;
    case OP_READ_OEM:
        rc = do_read_region(OEM_BASE, "OEM config", path);
        break;
    default:
        break;
    }

    hid_close();
    return rc;
}
