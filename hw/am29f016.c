/*
 * AM29F016 flash emulation
 *
 * Copyright (c) 2009 AdaCore
 *
 * Written by Tristan Gingold.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 * This specific version of flash was needed for es40 because its flash is
 * not memory-mapped and the stride is particular: one byte of flash for
 * 0x40 bytes in io.
 *
 * Please use pflash_cfiX.c if you need a flash that is mapped in memory.
 */
#include "sysemu.h"
#include "flash.h"
#include "block.h"
#include "qemu-timer.h"

//#define DEBUG_FLASH

struct Am29f016State {
    BlockDriverState *bs;
    QEMUTimer *timer;
    int wr_start;
    int wr_end;
    unsigned char cycle;
    unsigned char cmd;
    unsigned char counter;
    unsigned char prot[8];
    unsigned char mem[0x200000];
};

static void am29f016_update(Am29f016State *flash, int offset, int size)
{
    int len;
    int res;

    if (!flash->bs)
        return;

    len = (size + (offset & 511) + 511) >> 9;
    res = bdrv_write(flash->bs, offset >> 9, flash->mem + (offset & ~511), len);
    qemu_log("am29f016_update: off=0x%06x, size=%d, len=%d, res=%d\n",
             offset, size, len, res);
}

static void am29f016_flush(Am29f016State *flash)
{
    if (!flash->bs || flash->wr_start < 0)
        return;

    am29f016_update(flash,
                    flash->wr_start, flash->wr_end - flash->wr_start + 1);
    flash->wr_start = -1;
}

static void am29f016_timer(void *opaque)
{
    am29f016_flush(opaque);
}

uint8_t am29f016_readb(Am29f016State *s, uint32_t addr)
{
    uint32_t ret;

    if (s->cycle == 0)
        ret = s->mem[addr];
    else if (s->cycle == 2 && s->cmd == 0x90) {
        /* Auto select.  */
        switch ((addr & 0xff)) {
        case 0:
            ret = 0x01;
            break;
        case 1:
            ret = 0xad;
            break;
        case 2:
            ret = s->prot[(addr >> 18) & 7];
            break;
        default:
            qemu_log("am29f016: bad autoselect read addr=%06x\n",
                     (unsigned int) addr);
            ret = 0;
        }
    }
    else if (s->cycle == 5 && s->cmd == 0x80) {
        /* Erase.  */
        if (s->counter-- == 0) {
            s->cycle = 0;
            s->cmd = 0;
            ret = 0x80;
        }
        else
            ret = 0;
    }
    else {
        qemu_log("am29f016: read in cycle=%d cmd=0x%02x\n", s->cycle, s->cmd);
        ret = 0;
    }

#ifdef DEBUG_FLASH
    qemu_log("am29f016 read  addr=%06x, val=%02x (cmd=%02x, cyc=%d)\n",
             (unsigned int)addr, ret, s->cmd, s->cycle);
#endif
    return ret;
}

void am29f016_writeb(Am29f016State *s, uint32_t addr, uint8_t value)
{
    unsigned int ad;

    ad = addr & 0xffff;

#ifdef DEBUG_FLASH
    qemu_log("am29f016 write addr=%06x, val=%02x\n",
             (unsigned int)addr, value);
#endif

    switch (s->cycle) {
    case 0:
        if (value == 0xF0)
            s->cycle = 0;
        else if (ad == 0x5555 && value == 0xaa)
            s->cycle = 1;
        else
            qemu_log("am29f016: bad write in cycle0: addr=%05x data=%02x\n",
                     (unsigned int)addr, value);
        break;
    case 1:
        if (ad == 0x2aaa && value == 0x55)
            s->cycle = 2;
        else {
            qemu_log("am29f016: bad write in cycle1: addr=%05x data=%02x\n",
                     (unsigned int)addr, value);
            s->cycle = 0;
        }
        break;
    case 2:
        if (ad == 0x5555) {
            if (value == 0xF0) {
                s->cmd = 0;
                s->cycle = 0;
                break;
            }
            else if (value == 0x90) {
                s->cmd = value;
                break;
            }
            else if (value == 0xa0 || value == 0x80) {
                s->cmd = value;
                s->cycle = 3;
                break;
            }
            else if (value == 0xaa && s->cmd == 0x90) {
                s->cycle = 1;
                break;
            }
        }
        qemu_log("am29f016: bad write in cycle2: addr=%05x data=%02x\n",
                 (unsigned int)addr, value);
        s->cycle = 0;
        break;
    case 3:
        if (s->cmd == 0xa0) {
            /* Write */
            s->mem[addr] &= value;
            s->cycle = 0;
            s->cmd = 0;
            if (s->wr_start >= 0 && addr == s->wr_end + 1) {
                s->wr_end++;
            } else if (s->wr_start >= 0 && s->wr_start - 1 == addr) {
                s->wr_start--;
            } else {
                if (s->wr_start >= 0)
                    am29f016_flush(s);
                /* Let's wait 1 second before writing on disk.  */
                s->wr_start = addr;
                s->wr_end = addr;
                qemu_mod_timer(s->timer, qemu_get_clock(rt_clock) + 1000);
            }
        }
        else if (s->cmd == 0x80 && ad == 0x5555 && value == 0xaa)
            s->cycle = 4;
        else {
            qemu_log("am29f016: bad write in cycle3: addr=%05x data=%02x\n",
                     (unsigned int)addr, value);
            s->cycle = 0;
        }
        break;
    case 4:
        if (ad == 0x2aaa && value == 0x55)
            s->cycle = 5;
        else {
            qemu_log("am29f016: bad write in cycle4: addr=%05x data=%02x\n",
                     (unsigned int)addr, value);
            s->cycle = 0;
        }
        break;
    case 5:
        if (value == 0x10 && ad == 0x5555) {
            /* Erase chip. */
            memset(s->mem, 0xff, sizeof(s->mem));
            s->counter = 10;
            am29f016_flush(s);
            am29f016_update(s, 0, sizeof(s->mem));
        }
        else if (value == 0x30) {
            /* Erase sector. */
            qemu_log("am29f016: erasing sector %d\n", (int)(addr >> 16));
            memset(&s->mem[addr & 0x1f0000], 0xff, 0x10000);
            s->counter = 4;
            am29f016_flush(s);
            am29f016_update(s, addr & 0x1f0000, 0x10000);
        }
        else {
            qemu_log("am29f016: bad write in cycle5: addr=%06x data=%02x\n",
                     (unsigned int)addr, value);
            s->cycle = 0;
        }
        break;
    default:
        qemu_log("am29f016: unhandled write to reg=0x%06x (%08x)\n",
                 (unsigned int)addr, value);
        break;
    }
}

Am29f016State *am29f016_init(BlockDriverState *bs)
{
    Am29f016State *res;

    res = qemu_mallocz(sizeof(Am29f016State));
    res->bs = bs;
    if (bs) {
        if (bdrv_read(res->bs, 0, res->mem, sizeof(res->mem) >> 9) < 0) {
            fprintf(stderr, "am29f016: cannot read content\n");
            exit(1);
        }
    }
    res->wr_start = -1;
    res->timer = qemu_new_timer(rt_clock, am29f016_timer, res);

    return res;
}
