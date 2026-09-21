/* integrive_hw.c -- private hardware layer (see integrive_hw.h). */
#define _GNU_SOURCE
#include "integrive_hw.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile uint32_t *map_window(int fd, uint32_t base)
{
    void *p = mmap(NULL, ITG_WINDOW_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)base);
    return (p == MAP_FAILED) ? NULL : (volatile uint32_t *)p;
}

int itg_hw_open(itg_hw_t *hw)
{
    memset(hw, 0, sizeof(*hw));
    hw->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (hw->mem_fd < 0)
        return -1;

    hw->tx  = map_window(hw->mem_fd, ITG_TX_INTF_BASE);
    hw->rx  = map_window(hw->mem_fd, ITG_RX_INTF_BASE);
    hw->xpu = map_window(hw->mem_fd, ITG_XPU_BASE);
    if (!hw->tx || !hw->rx || !hw->xpu) {
        itg_hw_close(hw);
        return -1;
    }
    return 0;
}

void itg_hw_close(itg_hw_t *hw)
{
    if (hw->tx)  munmap((void *)hw->tx,  ITG_WINDOW_LEN);
    if (hw->rx)  munmap((void *)hw->rx,  ITG_WINDOW_LEN);
    if (hw->xpu) munmap((void *)hw->xpu, ITG_WINDOW_LEN);
    if (hw->mem_fd >= 0) close(hw->mem_fd);
    memset(hw, 0, sizeof(*hw));
    hw->mem_fd = -1;
}

int itg_sysfs_write_str(const char *dir, const char *attr, const char *val)
{
    char path[512];
    FILE *f;
    int   rc;

    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    f = fopen(path, "w");
    if (!f)
        return -1;
    rc = (fputs(val, f) >= 0) ? 0 : -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

int itg_sysfs_write_ll(const char *dir, const char *attr, long long val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", val);
    return itg_sysfs_write_str(dir, attr, buf);
}

int itg_sysfs_read_str(const char *dir, const char *attr, char *buf, size_t len)
{
    char  path[512];
    FILE *f;
    char *nl;

    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    return 0;
}

int itg_sysfs_read_dbl(const char *dir, const char *attr, double *out)
{
    char buf[64];
    if (itg_sysfs_read_str(dir, attr, buf, sizeof(buf)) != 0)
        return -1;
    *out = strtod(buf, NULL);
    return 0;
}
