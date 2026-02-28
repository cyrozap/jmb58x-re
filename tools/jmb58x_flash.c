/*
 * jmb58x_flash.c — JMB58x SPI flash dump / program / verify tool
 *
 * Uses the SPI controller registers in PCI BAR5 (AHCI Vendor Specific range)
 * to communicate with the SPI flash chip soldered to a JMB582/JMB585.
 *
 * Register map derived from:
 *   https://github.com/cyrozap/jmb58x-re  (data/regs-jmb58x.yaml)
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o jmb58x_flash jmb58x_flash.c
 *
 * Usage:
 *   jmb58x_flash [options] <command> [file]
 *
 * Commands:
 *   info                  — show JEDEC ID and detected flash size
 *   dump   <output_file>  — read entire flash to file
 *   write  <input_file>   — erase + program flash from file
 *   verify <input_file>   — compare flash content against file
 *   erase                 — chip erase
 *
 * Options:
 *   -d <BDF>        PCI device address (e.g. 01:00.0). Auto-detected if omitted.
 *   -s <size>       Flash size override in bytes (decimal or 0xHEX).
 *   -b              Use /dev/mem with BAR5 physical address (fallback mode).
 *   -S              Swap byte order of each 32-bit SPI data word (try if data
 *                   looks garbled).
 *   -h              Show help.
 *
 * SPDX-License-Identifier: 0BSD
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  BAR5 register offsets (from regs-jmb58x.yaml)                     */
/* ------------------------------------------------------------------ */

#define BAR5_SPI_CFG0       0x00B4
#define BAR5_SPI_CFG1       0x00B8
#define BAR5_BUS_ADDRESS    0x00C0
#define BAR5_SPI_DATA       0x00CC

/* ------------------------------------------------------------------ */
/*  SPI_CFG0 field helpers                                             */
/* ------------------------------------------------------------------ */

/*
 * Bits [31:24]  CMD          — SPI command byte
 * Bits [23:22]  DATA_LEN     — (number of data bytes − 1)  0..3 → 1..4 bytes
 * Bits [21:20]  ADDR_LEN     — (number of addr bytes − 1)  0..3 → 1..4 bytes
 * Bit  [19]     DATA_RX_EN   — receive data from SPI into SPI_DATA
 * Bit  [18]     DATA_TX_EN   — transmit data from SPI_DATA to SPI
 * Bit  [17]     DUMMY_TX_EN  — send one dummy byte after address
 * Bit  [16]     ADDR_TX_EN   — send address bytes from BUS_ADDRESS
 * Bits [15:8]   SPI_UNK      — unknown, normally 0x80
 * Bits [7:4]    CLK_DIV      — SPI clock = 25 MHz / (2*(CLK_DIV+1))
 * Bits [3:0]    SCK_CSN_DELAY
 */

static inline uint32_t
spi_cfg0(uint8_t cmd,
         int addr_tx, int addr_len,
         int dummy_tx,
         int data_tx, int data_rx, int data_len,
         int clk_div)
{
    uint32_t v = 0;
    v |= (uint32_t)cmd          << 24;
    v |= (uint32_t)(data_len & 3) << 22;
    v |= (uint32_t)(addr_len & 3) << 20;
    if (data_rx)  v |= 1u << 19;
    if (data_tx)  v |= 1u << 18;
    if (dummy_tx) v |= 1u << 17;
    if (addr_tx)  v |= 1u << 16;
    v |= 0x80u << 8;                       /* SPI_UNK */
    v |= (uint32_t)(clk_div & 0xF) << 4;
    return v;
}

/* ------------------------------------------------------------------ */
/*  Standard SPI flash commands                                        */
/* ------------------------------------------------------------------ */

#define SPICMD_READ         0x03
#define SPICMD_RDID         0x9F
#define SPICMD_RDSFDP       0x5A
#define SPICMD_RDSR         0x05
#define SPICMD_WREN         0x06
#define SPICMD_WRDI         0x04
#define SPICMD_PP           0x02    /* Page Program (256 B max) */
#define SPICMD_SE_4K        0x20    /* Sector Erase   4 KB */
#define SPICMD_BE_64K       0xD8    /* Block  Erase  64 KB */
#define SPICMD_CE           0xC7    /* Chip   Erase        */

/* Status register bits */
#define SR_WIP              0x01    /* Write In Progress */
#define SR_WEL              0x02    /* Write Enable Latch */

/* ------------------------------------------------------------------ */
/*  Known JEDEC manufacturer IDs (first byte)                          */
/* ------------------------------------------------------------------ */

struct flash_id {
    uint8_t mfr;
    const char *mfr_name;
};

static const struct flash_id known_mfrs[] = {
    { 0xEF, "Winbond"     },
    { 0xC8, "GigaDevice"  },
    { 0xC2, "Macronix"    },
    { 0x1F, "Adesto/Atmel"},
    { 0x20, "Micron/Numonyx"},
    { 0xBF, "Microchip/SST"},
    { 0x01, "Spansion/Cypress"},
    { 0x9D, "ISSI"        },
    { 0x68, "Boya"        },
    { 0x85, "Puya"        },
    { 0x5E, "Zbit"        },
    { 0x0B, "XTX"         },
    { 0,    NULL           },
};

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

static volatile uint32_t *g_bar5;   /* pointer to mmap'd BAR5 MMIO     */
static int  g_swap_bytes;           /* byte-swap 32-bit SPI data words  */
static int  g_clk_div = 1;         /* CLK_DIV value: 6.25 MHz default  */
static int  g_verbose;

/* ------------------------------------------------------------------ */
/*  Low-level BAR5 MMIO helpers                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t mmio_read32(uint32_t offset)
{
    return *(volatile uint32_t *)((volatile uint8_t *)g_bar5 + offset);
}

static inline void mmio_write32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((volatile uint8_t *)g_bar5 + offset) = val;
}

static inline uint32_t bswap32(uint32_t v)
{
    return ((v >> 24) & 0x000000FFu) |
           ((v >>  8) & 0x0000FF00u) |
           ((v <<  8) & 0x00FF0000u) |
           ((v << 24) & 0xFF000000u);
}

/* ------------------------------------------------------------------ */
/*  SPI controller primitives                                          */
/* ------------------------------------------------------------------ */

/*
 * Trigger an SPI transaction by reading SPI_DATA (for RX) or writing (for TX /
 * command-only).  SPI_CFG0 and BUS_ADDRESS must already be set up.
 */

static uint32_t spi_trigger_read(void)
{
    uint32_t v = mmio_read32(BAR5_SPI_DATA);
    return g_swap_bytes ? bswap32(v) : v;
}

static void spi_trigger_write(uint32_t data)
{
    mmio_write32(BAR5_SPI_DATA, g_swap_bytes ? bswap32(data) : data);
}

/* ---- high-level SPI flash operations -------------------------------- */

static uint32_t spi_read_jedec_id(void)
{
    /* RDID: cmd 0x9F, receive 3 bytes (data_len = 2) */
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_RDID,
                           /*addr_tx*/0, /*addr_len*/0,
                           /*dummy*/0,
                           /*data_tx*/0, /*data_rx*/1, /*data_len*/2,
                           g_clk_div));
    return spi_trigger_read();
}

static uint8_t spi_read_status(void)
{
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_RDSR,
                           0, 0, 0,
                           0, 1, 0,     /* rx 1 byte */
                           g_clk_div));
    return (uint8_t)(spi_trigger_read() & 0xFF);
}

static void spi_write_enable(void)
{
    /* WREN: command only, no addr, no data */
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_WREN,
                           0, 0, 0,
                           0, 0, 0,
                           g_clk_div));
    spi_trigger_write(0);
}

static void spi_wait_ready(int timeout_ms)
{
    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (spi_read_status() & SR_WIP) {
        usleep(500);
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        long elapsed = (ts_now.tv_sec - ts_start.tv_sec) * 1000 +
                       (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            fprintf(stderr, "ERROR: SPI flash busy timeout (%d ms)\n",
                    timeout_ms);
            exit(1);
        }
    }
}

/*
 * Read 4 bytes from flash at a 4-byte-aligned address.
 * Returns data in host byte order such that the byte at `addr` is in the
 * least-significant byte of the returned uint32_t.
 */
static uint32_t spi_read_4bytes(uint32_t addr)
{
    mmio_write32(BAR5_BUS_ADDRESS, addr & 0x00FFFFFC);
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_READ,
                           /*addr_tx*/1, /*addr_len*/2,   /* 3 addr bytes */
                           /*dummy*/0,
                           /*data_tx*/0, /*data_rx*/1, /*data_len*/3,  /* 4 data bytes */
                           g_clk_div));
    return spi_trigger_read();
}

/*
 * Page-program 4 bytes at a 4-byte-aligned address.
 * `data` layout: byte at `addr` is in the least-significant byte.
 */
static void spi_page_program_4bytes(uint32_t addr, uint32_t data)
{
    spi_write_enable();

    mmio_write32(BAR5_BUS_ADDRESS, addr & 0x00FFFFFC);
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_PP,
                           1, 2,        /* 3 addr bytes */
                           0,
                           1, 0, 3,     /* tx 4 data bytes */
                           g_clk_div));
    spi_trigger_write(data);

    spi_wait_ready(50);     /* page program typically < 5 ms */
}

static void spi_sector_erase_4k(uint32_t addr)
{
    spi_write_enable();

    mmio_write32(BAR5_BUS_ADDRESS, addr & 0x00FFF000);
    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_SE_4K,
                           1, 2,        /* 3 addr bytes */
                           0,
                           0, 0, 0,
                           g_clk_div));
    spi_trigger_write(0);

    spi_wait_ready(1000);   /* sector erase typically < 400 ms */
}

static void spi_chip_erase(void)
{
    spi_write_enable();

    mmio_write32(BAR5_SPI_CFG0,
                 spi_cfg0(SPICMD_CE,
                           0, 0, 0,
                           0, 0, 0,
                           g_clk_div));
    spi_trigger_write(0);

    spi_wait_ready(120000); /* chip erase up to ~100 s for large flash */
}

/* ------------------------------------------------------------------ */
/*  JEDEC ID → flash size helper                                       */
/* ------------------------------------------------------------------ */

static uint32_t jedec_id_to_size(uint32_t jedec)
{
    /*
     * Byte layout in `jedec` (little-endian from spi_trigger_read):
     *   bits [ 7: 0]  = manufacturer ID
     *   bits [15: 8]  = memory type
     *   bits [23:16]  = capacity code
     *
     * Common capacity codes: 0x11=128K, 0x12=256K, 0x13=512K,
     *                        0x14=1M,   0x15=2M,   0x16=4M, …
     */
    uint8_t cap = (jedec >> 16) & 0xFF;
    if (cap >= 0x10 && cap <= 0x21)
        return 1u << cap;                  /* 2^cap bytes */
    return 0;                              /* unknown */
}

static const char *jedec_mfr_name(uint8_t mfr)
{
    for (const struct flash_id *p = known_mfrs; p->mfr_name; p++)
        if (p->mfr == mfr)
            return p->mfr_name;
    return "Unknown";
}

/* ------------------------------------------------------------------ */
/*  PCI device discovery via sysfs                                     */
/* ------------------------------------------------------------------ */

#define JMB58X_VID  0x197b
#define JMB585_DID  0x0585
#define JMB582_DID  0x0582

/*
 * Try to find a JMB58x device in /sys/bus/pci/devices/.
 * On success, writes the slot name (e.g. "0000:01:00.0") to `buf`.
 * Returns 0 on success, -1 on failure.
 */
static int find_jmb58x_slot(char *buf, size_t bufsz)
{
    DIR *dp = opendir("/sys/bus/pci/devices");
    if (!dp) return -1;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char path[512];
        uint32_t vid = 0, did = 0;

        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/vendor", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%i", &vid) != 1) vid = 0;
            fclose(f);
        }

        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/device", de->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%i", &did) != 1) did = 0;
            fclose(f);
        }

        if (vid == JMB58X_VID && (did == JMB585_DID || did == JMB582_DID)) {
            size_t namelen = strlen(de->d_name);
            if (namelen >= bufsz) namelen = bufsz - 1;
            memcpy(buf, de->d_name, namelen);
            buf[namelen] = '\0';
            closedir(dp);
            return 0;
        }
    }
    closedir(dp);
    return -1;
}

/*
 * Open and mmap BAR5 via sysfs resource5 file.
 * Returns mmap pointer on success, MAP_FAILED on error.
 */
static volatile uint32_t *mmap_bar5_sysfs(const char *slot)
{
    char path[512];

    /* Determine full slot name: accept "01:00.0" → "0000:01:00.0" */
    char full_slot[64];
    if (strlen(slot) <= 7) {
        snprintf(full_slot, sizeof(full_slot), "0000:%s", slot);
    } else {
        snprintf(full_slot, sizeof(full_slot), "%s", slot);
    }

    /* Enable memory access via the PCI command register just in case */
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%s/enable", full_slot);
    FILE *ef = fopen(path, "w");
    if (ef) { fprintf(ef, "1"); fclose(ef); }

    /* Open resource5 */
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%s/resource5", full_slot);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "       Make sure you run as root.\n");
        return MAP_FAILED;
    }

    /* Get file size (= BAR size) */
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        /* If stat doesn't give size, assume 8 KiB (typical for JMB585 BAR5) */
        st.st_size = 8192;
    }

    volatile uint32_t *ptr = mmap(NULL, (size_t)st.st_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap resource5 failed: %s\n", strerror(errno));
    }
    return ptr;
}

/*
 * Open and mmap BAR5 via /dev/mem (fallback).
 */
static volatile uint32_t *mmap_bar5_devmem(const char *slot)
{
    char path[512], line[256];

    char full_slot[64];
    if (strlen(slot) <= 7)
        snprintf(full_slot, sizeof(full_slot), "0000:%s", slot);
    else
        snprintf(full_slot, sizeof(full_slot), "%s", slot);

    /* Read BAR5 physical address from sysfs resource file */
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%s/resource", full_slot);
    FILE *rf = fopen(path, "r");
    if (!rf) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
        return MAP_FAILED;
    }

    /* resource file: one line per BAR.  BAR5 is line index 5. */
    uint64_t bar5_start = 0, bar5_end = 0;
    for (int i = 0; i <= 5; i++) {
        if (!fgets(line, sizeof(line), rf)) {
            fprintf(stderr, "ERROR: Cannot read BAR5 from resource file.\n");
            fclose(rf);
            return MAP_FAILED;
        }
        if (i == 5) {
            uint64_t flags;
            if (sscanf(line, "%lx %lx %lx",
                       (unsigned long *)&bar5_start,
                       (unsigned long *)&bar5_end,
                       (unsigned long *)&flags) < 2) {
                fprintf(stderr, "ERROR: Cannot parse BAR5 line.\n");
                fclose(rf);
                return MAP_FAILED;
            }
        }
    }
    fclose(rf);

    if (bar5_start == 0) {
        fprintf(stderr, "ERROR: BAR5 address is zero — BAR not assigned?\n");
        return MAP_FAILED;
    }

    size_t bar5_size = (size_t)(bar5_end - bar5_start + 1);
    if (bar5_size < 0x100) bar5_size = 0x2000;

    printf("BAR5 phys: 0x%lx  size: 0x%lx\n",
           (unsigned long)bar5_start, (unsigned long)bar5_size);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open /dev/mem: %s\n", strerror(errno));
        return MAP_FAILED;
    }

    volatile uint32_t *ptr = mmap(NULL, bar5_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, (off_t)bar5_start);
    close(fd);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap /dev/mem failed: %s\n", strerror(errno));
    }
    return ptr;
}

/* ------------------------------------------------------------------ */
/*  Progress bar                                                       */
/* ------------------------------------------------------------------ */

static void progress(const char *label, uint32_t current, uint32_t total)
{
    if (total == 0) return;
    int pct = (int)((uint64_t)current * 100 / total);
    int w = 40;
    int filled = (int)((uint64_t)current * w / total);
    fprintf(stderr, "\r%s [", label);
    for (int i = 0; i < w; i++)
        fputc(i < filled ? '#' : '.', stderr);
    fprintf(stderr, "] %3d%%  (%u / %u)", pct, current, total);
    if (current == total) fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/*  Commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_info(uint32_t flash_size_override)
{
    uint32_t jedec = spi_read_jedec_id();
    uint8_t mfr  = (jedec >>  0) & 0xFF;
    uint8_t type = (jedec >>  8) & 0xFF;
    uint8_t cap  = (jedec >> 16) & 0xFF;
    uint32_t size = jedec_id_to_size(jedec);

    printf("JEDEC ID      : 0x%06X\n", jedec & 0xFFFFFF);
    printf("  Manufacturer: 0x%02X (%s)\n", mfr, jedec_mfr_name(mfr));
    printf("  Memory Type : 0x%02X\n", type);
    printf("  Capacity    : 0x%02X", cap);
    if (size)
        printf(" (%u KB / %u Mbit)", size / 1024, size * 8 / (1024 * 1024));
    printf("\n");

    if (flash_size_override)
        printf("Size override : %u bytes\n", flash_size_override);

    /* Also read the first 4 bytes of flash to show PCI IDs */
    uint32_t first4 = spi_read_4bytes(0);
    uint16_t did = (uint16_t)(first4 & 0xFFFF);
    uint16_t vid = (uint16_t)(first4 >> 16);
    printf("Flash[0:3]    : 0x%08X\n", first4);
    printf("  PCI DID:VID : %04x:%04x", did, vid);
    if (vid == JMB58X_VID && (did == JMB585_DID || did == JMB582_DID))
        printf("  (JMB%x — looks correct!)", did);
    else if (vid == 0xFFFF && did == 0xFFFF)
        printf("  (blank / erased flash)");
    else
        printf("  (unexpected — try -S to swap byte order?)");
    printf("\n");

    /* Read status register */
    uint8_t sr = spi_read_status();
    printf("Status Reg    : 0x%02X  (WIP=%d WEL=%d)\n",
           sr, !!(sr & SR_WIP), !!(sr & SR_WEL));

    return 0;
}

static int cmd_dump(const char *outfile, uint32_t flash_size)
{
    if (!flash_size) {
        uint32_t jedec = spi_read_jedec_id();
        flash_size = jedec_id_to_size(jedec);
        if (!flash_size) {
            fprintf(stderr,
                    "ERROR: Cannot determine flash size from JEDEC ID 0x%06X.\n"
                    "       Specify size with -s <bytes>.\n",
                    jedec & 0xFFFFFF);
            return 1;
        }
        fprintf(stderr, "Detected flash size: %u bytes (%u KB)\n",
                flash_size, flash_size / 1024);
    }

    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot create %s: %s\n",
                outfile, strerror(errno));
        return 1;
    }

    fprintf(stderr, "Dumping %u bytes to %s ...\n", flash_size, outfile);

    for (uint32_t addr = 0; addr < flash_size; addr += 4) {
        uint32_t data = spi_read_4bytes(addr);
        /* Write as little-endian 4 bytes */
        uint8_t buf[4];
        buf[0] = (uint8_t)(data >>  0);
        buf[1] = (uint8_t)(data >>  8);
        buf[2] = (uint8_t)(data >> 16);
        buf[3] = (uint8_t)(data >> 24);
        if (fwrite(buf, 1, 4, fp) != 4) {
            fprintf(stderr, "\nERROR: Write error: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }
        if ((addr & 0xFFF) == 0)
            progress("Dump ", addr, flash_size);
    }
    progress("Dump ", flash_size, flash_size);

    fclose(fp);
    fprintf(stderr, "Done.  Wrote %u bytes.\n", flash_size);
    return 0;
}

static int cmd_verify(const char *infile, uint32_t flash_size)
{
    FILE *fp = fopen(infile, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n",
                infile, strerror(errno));
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (flash_size == 0)
        flash_size = (uint32_t)file_size;

    if ((uint32_t)file_size < flash_size) {
        fprintf(stderr,
                "WARNING: File size (%ld) < flash size (%u). "
                "Will verify up to file size only.\n",
                file_size, flash_size);
        flash_size = (uint32_t)file_size;
    }

    /* Round up to multiple of 4 */
    uint32_t verify_len = (flash_size + 3) & ~3u;

    uint8_t *file_data = malloc(verify_len);
    if (!file_data) {
        fprintf(stderr, "ERROR: malloc(%u) failed\n", verify_len);
        fclose(fp);
        return 1;
    }
    memset(file_data, 0xFF, verify_len);
    if (fread(file_data, 1, flash_size, fp) != flash_size) {
        fprintf(stderr, "ERROR: Failed to read file: %s\n", strerror(errno));
        free(file_data);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    fprintf(stderr, "Verifying %u bytes against %s ...\n", flash_size, infile);

    int errors = 0;
    for (uint32_t addr = 0; addr < verify_len; addr += 4) {
        uint32_t flash_val = spi_read_4bytes(addr);
        uint32_t file_val = (uint32_t)file_data[addr]
                          | ((uint32_t)file_data[addr+1] << 8)
                          | ((uint32_t)file_data[addr+2] << 16)
                          | ((uint32_t)file_data[addr+3] << 24);
        if (flash_val != file_val) {
            if (errors < 32) {
                fprintf(stderr,
                        "  MISMATCH at 0x%06X: flash=0x%08X file=0x%08X\n",
                        addr, flash_val, file_val);
            }
            errors++;
        }
        if ((addr & 0xFFF) == 0)
            progress("Verify", addr, verify_len);
    }
    progress("Verify", verify_len, verify_len);

    free(file_data);

    if (errors) {
        fprintf(stderr, "VERIFY FAILED: %d mismatched dwords.\n", errors);
        return 1;
    }
    fprintf(stderr, "Verify OK — flash matches file.\n");
    return 0;
}

static int cmd_erase(uint32_t flash_size)
{
    if (!flash_size) {
        uint32_t jedec = spi_read_jedec_id();
        flash_size = jedec_id_to_size(jedec);
    }

    fprintf(stderr, "Performing chip erase...\n");
    spi_chip_erase();
    fprintf(stderr, "Chip erase complete.\n");

    /* Quick verify: read first 4 bytes, should be 0xFFFFFFFF */
    uint32_t v = spi_read_4bytes(0);
    if (v != 0xFFFFFFFF) {
        fprintf(stderr,
                "WARNING: Flash[0] = 0x%08X after erase (expected 0xFFFFFFFF).\n"
                "         Try -S flag to swap byte order, or check flash chip.\n",
                v);
    } else {
        fprintf(stderr, "Erase verified OK (first 4 bytes = 0xFFFFFFFF).\n");
    }

    (void)flash_size;
    return 0;
}

static int cmd_write(const char *infile, uint32_t flash_size)
{
    FILE *fp = fopen(infile, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n",
                infile, strerror(errno));
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "ERROR: File is empty.\n");
        fclose(fp);
        return 1;
    }

    if (flash_size == 0) {
        uint32_t jedec = spi_read_jedec_id();
        flash_size = jedec_id_to_size(jedec);
        if (!flash_size) {
            fprintf(stderr,
                    "ERROR: Cannot determine flash size. Use -s <bytes>.\n");
            fclose(fp);
            return 1;
        }
    }

    if ((uint32_t)file_size > flash_size) {
        fprintf(stderr,
                "ERROR: File size (%ld) exceeds flash size (%u).\n",
                file_size, flash_size);
        fclose(fp);
        return 1;
    }

    /* Read entire file into memory, pad to 4-byte boundary with 0xFF */
    uint32_t prog_len = ((uint32_t)file_size + 3) & ~3u;
    uint8_t *data = malloc(prog_len);
    if (!data) {
        fprintf(stderr, "ERROR: malloc(%u) failed\n", prog_len);
        fclose(fp);
        return 1;
    }
    memset(data, 0xFF, prog_len);
    if (fread(data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        fprintf(stderr, "ERROR: Failed to read file: %s\n", strerror(errno));
        free(data);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* ---- Step 1: Erase ---- */
    fprintf(stderr, "Step 1/3: Erasing flash (sector-by-sector, %u sectors)...\n",
            (prog_len + 0xFFF) / 0x1000);

    for (uint32_t addr = 0; addr < prog_len; addr += 0x1000) {
        spi_sector_erase_4k(addr);
        progress("Erase ", addr + 0x1000, ((prog_len + 0xFFF) & ~0xFFFu));
    }

    /* ---- Step 2: Program ---- */
    fprintf(stderr, "Step 2/3: Programming %u bytes...\n", prog_len);

    for (uint32_t addr = 0; addr < prog_len; addr += 4) {
        uint32_t word = (uint32_t)data[addr]
                      | ((uint32_t)data[addr+1] << 8)
                      | ((uint32_t)data[addr+2] << 16)
                      | ((uint32_t)data[addr+3] << 24);

        /* Skip writing 0xFFFFFFFF — already erased */
        if (word != 0xFFFFFFFF) {
            spi_page_program_4bytes(addr, word);
        }

        if ((addr & 0xFFF) == 0)
            progress("Program", addr, prog_len);
    }
    progress("Program", prog_len, prog_len);

    /* ---- Step 3: Verify ---- */
    fprintf(stderr, "Step 3/3: Verifying...\n");

    int errors = 0;
    for (uint32_t addr = 0; addr < prog_len; addr += 4) {
        uint32_t expected = (uint32_t)data[addr]
                          | ((uint32_t)data[addr+1] << 8)
                          | ((uint32_t)data[addr+2] << 16)
                          | ((uint32_t)data[addr+3] << 24);
        uint32_t actual = spi_read_4bytes(addr);
        if (actual != expected) {
            if (errors < 32)
                fprintf(stderr,
                        "  MISMATCH at 0x%06X: read=0x%08X expected=0x%08X\n",
                        addr, actual, expected);
            errors++;
        }
        if ((addr & 0xFFF) == 0)
            progress("Verify", addr, prog_len);
    }
    progress("Verify", prog_len, prog_len);

    free(data);

    if (errors) {
        fprintf(stderr,
                "WRITE FAILED: %d verification errors!\n", errors);
        return 1;
    }

    fprintf(stderr, "Write + verify OK.  %ld bytes programmed successfully.\n",
            file_size);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <command> [file]\n"
        "\n"
        "Commands:\n"
        "  info                  Show JEDEC ID and flash info\n"
        "  dump   <output_file>  Read entire flash to file\n"
        "  write  <input_file>   Erase + program + verify flash from file\n"
        "  verify <input_file>   Compare flash content against file\n"
        "  erase                 Chip erase (set all bytes to 0xFF)\n"
        "\n"
        "Options:\n"
        "  -d <BDF>    PCI slot (e.g. 01:00.0). Auto-detected if omitted.\n"
        "  -s <size>   Flash size override in bytes (e.g. 131072 or 0x20000).\n"
        "  -b          Use /dev/mem instead of sysfs resource5.\n"
        "  -S          Swap byte order of SPI data words.\n"
        "  -v          Verbose output.\n"
        "  -h          Show this help.\n"
        "\n"
        "Examples:\n"
        "  %s info\n"
        "  %s dump firmware_backup.bin\n"
        "  %s write new_firmware.bin\n"
        "  %s -d 01:00.0 -s 0x20000 dump backup.bin\n"
        "\n"
        "NOTE: Must be run as root.  The AHCI driver can remain loaded — this\n"
        "      tool only touches vendor-specific SPI registers in BAR5.\n"
        "\n",
        prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *slot_arg    = NULL;
    uint32_t    size_arg    = 0;
    int         use_devmem  = 0;

    /* Parse options */
    int opt;
    while ((opt = getopt(argc, argv, "d:s:bSvh")) != -1) {
        switch (opt) {
        case 'd': slot_arg    = optarg; break;
        case 's': size_arg    = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'b': use_devmem  = 1; break;
        case 'S': g_swap_bytes = 1; break;
        case 'v': g_verbose   = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "ERROR: No command specified.\n\n");
        usage(argv[0]);
        return 1;
    }

    const char *command = argv[optind];
    const char *file_arg = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    /* Determine PCI slot */
    char slot_buf[64];
    if (slot_arg) {
        snprintf(slot_buf, sizeof(slot_buf), "%s", slot_arg);
    } else {
        if (find_jmb58x_slot(slot_buf, sizeof(slot_buf)) < 0) {
            fprintf(stderr,
                    "ERROR: No JMB58x device found on PCI bus.\n"
                    "       Specify manually with -d <BDF>.\n");
            return 1;
        }
    }
    fprintf(stderr, "Using PCI device: %s\n", slot_buf);

    /* Map BAR5 */
    if (use_devmem)
        g_bar5 = mmap_bar5_devmem(slot_buf);
    else
        g_bar5 = mmap_bar5_sysfs(slot_buf);

    if (g_bar5 == MAP_FAILED)
        return 1;

    /* Set SPI clock multiplier to ×1 for safety */
    mmio_write32(BAR5_SPI_CFG1, 0);

    /* Dispatch command */
    int rc = 0;

    if (strcmp(command, "info") == 0) {
        rc = cmd_info(size_arg);
    } else if (strcmp(command, "dump") == 0) {
        if (!file_arg) {
            fprintf(stderr, "ERROR: dump requires an output filename.\n");
            rc = 1;
        } else {
            rc = cmd_dump(file_arg, size_arg);
        }
    } else if (strcmp(command, "write") == 0) {
        if (!file_arg) {
            fprintf(stderr, "ERROR: write requires an input filename.\n");
            rc = 1;
        } else {
            rc = cmd_write(file_arg, size_arg);
        }
    } else if (strcmp(command, "verify") == 0) {
        if (!file_arg) {
            fprintf(stderr, "ERROR: verify requires an input filename.\n");
            rc = 1;
        } else {
            rc = cmd_verify(file_arg, size_arg);
        }
    } else if (strcmp(command, "erase") == 0) {
        rc = cmd_erase(size_arg);
    } else {
        fprintf(stderr, "ERROR: Unknown command '%s'\n\n", command);
        usage(argv[0]);
        rc = 1;
    }

    return rc;
}