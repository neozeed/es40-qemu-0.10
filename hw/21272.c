/*
 * Qemu 21272 (Tsunami/Typhoon) chipset emulation.
 *
 * Copyright (c) 2009 AdaCore
 *
 * Written by Tristan Gingold.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include "hw.h"
#include "devices.h"
#include "pci.h"

//#define DEBUG_CCHIP
//#define DEBUG_PCHIP
//#define DEBUG_DCHIP
//#define DEBUG_PCICFG

typedef struct PchipState PchipState;
struct PchipState {
    /* IntAck handler.  */
    int (*iack_handler)(void *);
    void *iack_handler_param;

    PCIBus *pci;

    /* Pchip id.  */
    int num;

    /* Used to reconstruct 64bits accesses.  Low long word first.  */
    uint32_t data;

    uint32_t wsba[3];
    uint64_t wsba3;
    uint32_t wsm[3];
    uint32_t wsm3;
    uint64_t tba[3];
    uint64_t tba3;
    uint32_t perrmask;
    uint32_t plat;
    /* pctl */
    unsigned char ptevrfy;
    unsigned char mwin;
    unsigned char hole;
    unsigned char chaindis;
};

struct TyphoonState {
    qemu_irq *irqs;
    qemu_irq *intim_irq;
    CPUState *cpu[4];

    /* Used to reconstruct 64bits accesses.  Low long word first.  */
    uint32_t data;

    unsigned char misc_rev;
    unsigned char misc_abw;
    unsigned char misc_abt;

    int b_irq[4];

    uint64_t dim[4];
    uint64_t dir[4];
    uint64_t drir;
    uint64_t aar[4];

    /* dchip */
    uint64_t csc;
    uint64_t str;

    PchipState pchip[2];
};

static void illegal_write (void *opaque,
                            target_phys_addr_t addr, uint32_t value)
{
    qemu_log("illegal_write at addr="TARGET_FMT_lx"\n", addr);
}

static uint32_t illegal_read (void *opaque, target_phys_addr_t addr)
{
    qemu_log("illegal_read at addr="TARGET_FMT_lx"\n", addr);
    return 0;
}

static uint32_t typhoon_cchip_readl (void *opaque, target_phys_addr_t addr)
{
    TyphoonState *s = opaque;
    uint64_t val;
    int reg;

    /* Handle 64-bits accesses;  */
    if (addr & 0x04)
        return s->data;

    reg = addr >> 6;
    switch (reg) {
    case 0x00: /* CSC */
        val = s->csc;
        break;
    case 0x02: /* MISC */
        val = ((uint64_t)s->misc_rev << 32)
            | ((uint64_t)s->misc_abt << 20)
            | ((uint64_t)s->misc_abw << 16)
            | ((s->b_irq[0] & 4) << 2)
            | ((s->b_irq[1] & 4) << 3)
            | ((s->b_irq[2] & 4) << 4)
            | ((s->b_irq[3] & 4) << 5)
            | ((s->b_irq[0] & 8) << 5)
            | ((s->b_irq[1] & 8) << 6)
            | ((s->b_irq[2] & 8) << 7)
            | ((s->b_irq[3] & 8) << 8);
        break;
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        val = s->aar[reg - 4];
        break;
    case 0x08:
    case 0x09:
    case 0x18:
    case 0x19:
        val = s->dim[(reg & 1) | ((reg & 0x10) >> 3)];
        break;
    case 0x0a:
    case 0x0b:
    case 0x1a:
    case 0x1b:
        val = s->dir[(reg & 1) | ((reg & 0x10) >> 3)];
        break;
    case 0x0c:
        val = s->drir;
        break;
    default:
        qemu_log("21272: unhandled cchip read  reg=%x\n", reg);
        val = 0;
        break;
    }

#ifdef DEBUG_CCHIP
    fprintf(stderr,"typhoon cchip read  reg=%x, val=%016"PRIx64"\n", reg, val);
#endif
    s->data = val >> 32;
    return (uint32_t)val;
}

static void typhoon_cchip_writel (void *opaque,
                                  target_phys_addr_t addr, uint32_t value)
{
    TyphoonState *s = opaque;
    int reg;
    uint64_t val;

#if 0
    fprintf(stderr,"typhoon write addr="TARGET_FMT_lx", val=%08x\n",
            addr, value);
#endif

    /* Handle 64-bits accesses.  LSB first.  */
    if (!(addr & 0x04)) {
        s->data = value;
        return;
    }
    else
        val = ((uint64_t)value << 32) | s->data;

    reg = addr >> 6;

#ifdef DEBUG_CCHIP
    fprintf(stderr,"typhoon cchip write reg=%x, val=%016"PRIx64"\n", reg, val);
#endif

    switch (reg) {
    case 0x00: /* CSC */
        s->csc = (s->csc & 0xffffU) | (val & 0x0777777fff3f0000ULL);
        break;
    case 0x02: /* MISC */
        if (val & (1 << 24)) {
            /* ACL: arbitration clear.  */
            s->misc_abt = 0;
            s->misc_abw = 0;
        }
        if (val & (0xf << 12)) {
            int i;
            for (i = 0; i < 4 && s->cpu[i]; i++)
                if ((val & (0x1000 << i))) {
                    s->b_irq[i] &= ~(1 << 3);
                    cpu_alpha_update_irq(s->cpu[i], s->b_irq[i]);
                }
        }
        if ((val & (0xf << 16)) && s->misc_abw == 0) {
            /* ABW: arbitration won.  */
            s->misc_abw = (val >> 16) & 0x0f;
        }
        if ((val & (0x0f << 20)) && s->misc_abt == 0) {
            /* ABT: arbitration try.  */
            s->misc_abt = (val >> 20) & 0x0f;
        }
        if (val & (1ULL << 28)) {
            /* NXM - not handled.  */
        }
        if (val & (0x0fULL << 40)) {
            /* DEVUP - not handled.  */
        }
        if (val & (0x0f << 4)) {
            /* ITINTR */
            int i;
            for (i = 0; i < 4 && s->cpu[i]; i++)
                if ((val & (0x10 << i))) {
                    s->b_irq[i] &= ~(1 << 2);
                    cpu_alpha_update_irq(s->cpu[i], s->b_irq[i]);
                }
        }
        if (val & ~((0xfULL << 40) | (0xffULL << 32) | (1 << 28) | (1ULL << 24)
                    | (0x0f << 12) | (0xf << 16)
                    | (0x0f << 20) | (0x0f << 4))) {
            qemu_log("21272: unhandled value %016"PRIx64" written in MISC\n",
                     val);
        }
        break;
    case 0x08:
    case 0x09:
    case 0x18:
    case 0x19:
        s->dim[(reg & 1) | ((reg & 0x10) >> 3)] = val;
        break;
    default:
        qemu_log("21272: unhandled cchip write reg %x (%016"PRIx64")\n",
                 reg, val);
        break;
    }
}

static CPUReadMemoryFunc *typhoon_cchip_read[] = {
    &illegal_read,
    &illegal_read,
    &typhoon_cchip_readl,
};

static CPUWriteMemoryFunc *typhoon_cchip_write[] = {
    &illegal_write,
    &illegal_write,
    &typhoon_cchip_writel,
};

static uint32_t typhoon_dchip_readl (void *opaque, target_phys_addr_t addr)
{
    TyphoonState *s = opaque;
    uint64_t val;
    int reg;

    /* Handle 64-bits accesses;  */
    if (addr & 0x04)
        return s->data;

    reg = addr >> 6;
    switch (reg) {
    case 0x20: /* DSC */
        val = (s->csc & 0x3f) | ((s->csc >> (14 - 6)) & 0x40);
        val = (val << 8) | val;
        val = (val << 16) | val;
        val = (val << 32) | val;
        break;
    case 0x21: /* STR */
        val = s->str;
        break;
    case 0x22: /* DREV */
        val = 0x01 * 0x0101010101010101ULL;
        break;
    case 0x23: /* DSC2 */
        val = 0;
    default:
        qemu_log("21272: unhandled dchip read  reg=%x\n", reg);
        val = 0;
        break;
    }

#ifdef DEBUG_DCHIP
    fprintf(stderr,"typhoon dchip read  reg=%x, val=%016"PRIx64"\n", reg, val);
#endif
    s->data = val >> 32;
    return (uint32_t)val;
}

static void typhoon_dchip_writel (void *opaque,
                                  target_phys_addr_t addr, uint32_t value)
{
    TyphoonState *s = opaque;
    int reg;
    uint64_t val;

#if 0
    fprintf(stderr,"typhoon write addr="TARGET_FMT_lx", val=%08x\n",
            addr, value);
#endif

    /* Handle 64-bits accesses.  LSB first.  */
    if (!(addr & 0x04)) {
        s->data = value;
        return;
    }
    else
        val = ((uint64_t)value << 32) | s->data;

    reg = addr >> 6;

#ifdef DEBUG_DCHIP
    fprintf(stderr,"typhoon dchip write reg=%x, val=%016"PRIx64"\n", reg, val);
#endif

    switch (reg) {
    default:
        qemu_log("21272: unhandled dchip write reg %x (%016"PRIx64")\n",
                 reg, val);
        break;
    }
}

static CPUReadMemoryFunc *typhoon_dchip_read[] = {
    &illegal_read,
    &illegal_read,
    &typhoon_dchip_readl,
};

static CPUWriteMemoryFunc *typhoon_dchip_write[] = {
    &illegal_write,
    &illegal_write,
    &typhoon_dchip_writel,
};

static uint32_t typhoon_pchip_readl (void *opaque, target_phys_addr_t addr)
{
    PchipState *s = opaque;
    uint64_t val;
    int reg;

    /* Handle 64-bits accesses;  */
    if (addr & 0x04)
        return s->data;

    reg = addr >> 6;
    switch (reg) {
    case 0x00:
    case 0x01:
    case 0x02:
        val = s->wsba[reg];
        break;
    case 0x03:
        val = s->wsba3;
        break;
    case 0x04:
    case 0x05:
    case 0x06:
        val = s->wsm[reg - 4];
        break;
    case 0x07:
        val = s->wsm3;
        break;
    case 0x08:
    case 0x09:
    case 0x0a:
        val = s->tba[reg - 0x8];
        break;
    case 0x0b:
        val = s->tba3;
        break;
    case 0x0c: /* pctl  */
        val = (((uint64_t)(s->num & 3)) << 46)
            | (((uint64_t)s->ptevrfy) << 44)
            | (1ULL << 40) /* PCLKX */
            | (1 << 24) /* Rev */
            | (s->mwin << 6)
            | (s->hole << 5)
            | (s->chaindis << 3);
        break;
    case 0x0d: /* plat */
        val = s->plat;
        break;
    case 0x0f: /* PERROR */
        val = 0;  /* Not emulated.  */
        break;
    case 0x10: /* PERRMASK */
        val = s->perrmask;
        break;
    case 0x20: /* WO. */
        val = 0;
        break;
    default:
        qemu_log("21272: unhandled pchip read  reg=%x\n", reg);
        val = 0;
        break;
    }

#ifdef DEBUG_PCHIP
    fprintf(stderr,"typhoon pchip%d read  reg=%x, val=%016"PRIx64"\n",
            s->num, reg, val);
#endif
    s->data = val >> 32;
    return (uint32_t)val;
}

static void typhoon_pchip_writel (void *opaque,
                                  target_phys_addr_t addr, uint32_t value)
{
    PchipState *s = opaque;
    int reg;
    uint64_t val;

    /* Handle 64-bits accesses.  LSB first.  */
    if (!(addr & 0x04)) {
        s->data = value;
        return;
    }
    else
        val = ((uint64_t)value << 32) | s->data;

    reg = addr >> 6;

#ifdef DEBUG_PCHIP
    fprintf(stderr,"typhoon pchip%d write reg=%x, val=%016"PRIx64"\n",
            s->num, reg, val);
#endif

    switch (reg) {
    case 0:
    case 1:
    case 2:
        s->wsba[reg] = val & 0xfff00003;
        if (val & 1)
            qemu_log("21272: enabling wsba%d!\n", reg); /* Not yet emulated */
        break;
    case 3:
        s->wsba3 = val & 0xffffff80fff00003ULL;
        if (val & 1)
            qemu_log("21272: enabling wsba3!\n"); /* Not yet emulated */
        break;
    case 0x4:
    case 0x5:
    case 0x6:
        s->wsm[reg - 4] = val & 0xfff00000;
        break;
    case 0x7:
        s->wsm3 = val & 0xfff00000;
        break;
    case 0x8:
    case 0x9:
    case 0xa:
        s->tba[reg - 0x8] = val & 0x7fffffc00;
        break;
    case 0xb:
        s->tba3 = val & 0x7fffffc00;
        break;
    case 0x0c: /* pctl  */
        s->ptevrfy = (val >> 44) & 1;
        s->mwin = (val >> 6) & 1;
        s->hole = (val >> 5) & 1;
        s->chaindis = (val >> 3) & 1;
        if (val & ((1ULL << 43) | (1ULL << 42) | (3ULL << 36) | (0x0fULL << 32)
                   | (0x0f << 20) | (1 << 19) | (0x3f << 8)
                   | (1 << 2) | (1 << 1)))
            qemu_log("21272: pchip pctl: unhandled value %016"PRIx64"\n",
                     val);
        break;
    case 0x0d: /* plat */
        s->plat = val & 0xff00;
        break;
    case 0x0f: /* PERROR */
        break; /* Not emulated.  */
    case 0x10: /* PERRMASK */
        s->perrmask = val & 0xfff;
        break;
    case 0x13: /* tlbia */
        /* FIXME: todo */
        break;
    case 0x20: /* SPRST */
        /* Software pci-reset.  FIXME: disable bus?  */
        break;
    default:
        qemu_log("21272: unhandled pchip write reg=%x (%016"PRIx64")\n",
                 reg, val);
        break;
    }
}

static CPUReadMemoryFunc *typhoon_pchip_read[] = {
    &illegal_read,
    &illegal_read,
    &typhoon_pchip_readl,
};

static CPUWriteMemoryFunc *typhoon_pchip_write[] = {
    &illegal_write,
    &illegal_write,
    &typhoon_pchip_writel,
};


static void pchip_pci_io_writeb (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
    cpu_outb(NULL, addr, value);
}

static uint32_t pchip_pci_io_readb (void *opaque, target_phys_addr_t addr)
{
    return cpu_inb(NULL, addr);
}

static void pchip_pci_io_writew (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
#ifdef TARGET_WORDS_BIGENDIAN
    value = bswap16(value);
#endif
    cpu_outw(NULL, addr, value);
}

static uint32_t pchip_pci_io_readw (void *opaque, target_phys_addr_t addr)
{
    uint32_t ret;

    ret = cpu_inw(NULL, addr);
#ifdef TARGET_WORDS_BIGENDIAN
    ret = bswap16(ret);
#endif
    return ret;
}

static void pchip_pci_io_writel (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
#ifdef TARGET_WORDS_BIGENDIAN
    value = bswap32(value);
#endif
    cpu_outl(NULL, addr, value);
}

static uint32_t pchip_pci_io_readl (void *opaque, target_phys_addr_t addr)
{
    uint32_t ret;

    ret = cpu_inl(NULL, addr);
#ifdef TARGET_WORDS_BIGENDIAN
    ret = bswap32(ret);
#endif
    return ret;
}

static CPUWriteMemoryFunc *pchip_pci_io_write[] = {
    &pchip_pci_io_writeb,
    &pchip_pci_io_writew,
    &pchip_pci_io_writel,
};

static CPUReadMemoryFunc *pchip_pci_io_read[] = {
    &pchip_pci_io_readb,
    &pchip_pci_io_readw,
    &pchip_pci_io_readl,
};

static void pchip_pci_cfg_writex (void *opaque, target_phys_addr_t addr,
                                  uint32_t value, int sz)
{
    uint32_t a = addr & 0xffffffU;
    PchipState *s = opaque;

#ifdef DEBUG_PCICFG
    fprintf(stderr, "pci_cfg write: addr=%06x, sz=%d %u:%u:%02x, val=%08x\n",
            a, sz,
            (a >> 11) & 0x1f, (a >> 8) & 0x7, a & 0xff, value);
#endif

    pci_data_write(s->pci, a, value, 1 << sz);
}

static void pchip_pci_cfg_writel (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
#ifdef TARGET_WORDS_BIGENDIAN
    value = bswap32(value);
#endif
    pchip_pci_cfg_writex(opaque, addr, value, 2);
}

static void pchip_pci_cfg_writew (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
#ifdef TARGET_WORDS_BIGENDIAN
    value = bswap16(value);
#endif
    pchip_pci_cfg_writex(opaque, addr, value, 1);
}

static void pchip_pci_cfg_writeb (void *opaque, target_phys_addr_t addr,
                                uint32_t value)
{
    pchip_pci_cfg_writex(opaque, addr, value, 0);
}

static uint32_t pchip_pci_cfg_readx (void *opaque, target_phys_addr_t addr,
                                     int sz)
{
    uint32_t a = addr & 0xffffffU;
    PchipState *s = opaque;
    uint32_t val;

    if (s->num == 0)
        val = pci_data_read(s->pci, a, 1 << sz);
    else {
        switch (sz) {
        case 0:
            val = 0xff;
            break;
        case 1:
            val = 0xffff;
            break;
        default:
        case 2:
            val = 0xffffffff;
            break;
        }
    }

#ifdef DEBUG_PCICFG
    fprintf(stderr, "pci_cfg  read: addr=%06x, sz=%d %u:%u:%02x, val=%08x\n",
            a, sz,
            (a >> 11) & 0x1f, (a >> 8) & 0x7, a & 0xff,
            val);
#endif

    return val;
}

static uint32_t pchip_pci_cfg_readb (void *opaque, target_phys_addr_t addr)
{
    return pchip_pci_cfg_readx(opaque, addr, 0);
}

static uint32_t pchip_pci_cfg_readw (void *opaque, target_phys_addr_t addr)
{
    return pchip_pci_cfg_readx(opaque, addr, 1);
}

static uint32_t pchip_pci_cfg_readl (void *opaque, target_phys_addr_t addr)
{
    return pchip_pci_cfg_readx(opaque, addr, 2);
}

static CPUWriteMemoryFunc *pchip_pci_cfg_write[] = {
    &pchip_pci_cfg_writeb,
    &pchip_pci_cfg_writew,
    &pchip_pci_cfg_writel,
};

static CPUReadMemoryFunc *pchip_pci_cfg_read[] = {
    &pchip_pci_cfg_readb,
    &pchip_pci_cfg_readw,
    &pchip_pci_cfg_readl,
};

static void pchip_pci_iack_writex (void *opaque,
                            target_phys_addr_t addr, uint32_t value)
{
    qemu_log("21272: pci iack addr=%08x, val=%08x\n", (uint32_t)addr, value);
}

static uint32_t pchip_pci_iack_readx (void *opaque, target_phys_addr_t addr)
{
    PchipState *s = opaque;
    uint32_t res;

    /* Ideally we should have a PCIBus interface.  */
    res = (*s->iack_handler)(s->iack_handler_param);
    //fprintf(stderr, "21272 iack: addr=%016"PRIx64", res=%d\n", addr, res);
    return res;
}

static CPUWriteMemoryFunc *pchip_pci_iack_write[] = {
    &pchip_pci_iack_writex,
    &pchip_pci_iack_writex,
    &pchip_pci_iack_writex,
};

static CPUReadMemoryFunc *pchip_pci_iack_read[] = {
    &pchip_pci_iack_readx,
    &pchip_pci_iack_readx,
    &pchip_pci_iack_readx,
};

static void typhoon_reset (void *opaque)
{
    TyphoonState *s = opaque;

    s->misc_rev = 8;
    s->misc_abw = 0;
}

static void cchip_set_irq(void *opaque, int irq, int level)
{
    TyphoonState *s = opaque;
    uint64_t mask;
    int i;

#if 0
    qemu_log("cchip_set_irq: irq=%d level=%d\n", irq, level);
#endif
    mask = 1ULL << irq;
    if (level)
        s->drir |= mask;
    else
        s->drir &= ~mask;

    for (i = 0; i < 4 && s->cpu[i]; i++) {
        int old = s->b_irq[i];
        s->dir[i] = s->drir & s->dim[i];
        if (s->dir[i] >> 58)
            s->b_irq[i] |= (1 << 0);
        else
            s->b_irq[i] &= ~(1 << 0);

        if (s->dir[i] << 8)
            s->b_irq[i] |= (1 << 1);
        else
            s->b_irq[i] &= ~(1 << 1);

#if 0
        qemu_log("cchip_set_irq: cpu[%d]: drir=%016"PRIx64" dim=%016"PRIx64" "
                 " dir=%016"PRIx64" birq=%02x\n",
                 i, s->drir, s->dim[i], s->dir[i], s->b_irq[i]);
#endif
        if (s->b_irq[i] != old)
            cpu_alpha_update_irq(s->cpu[i], s->b_irq[i]);
    }
}

static void intim_set_irq(void *opaque, int irq, int level)
{
    int i;
    TyphoonState *s = opaque;
    unsigned char old;

#if 0
    {
        static int cnt;
        cnt++;
        if ((cnt % 1024) == 1023)
            fprintf(stderr, "Clock!\n");
    }
#endif

    if (0 && (s->b_irq[0] & 0x04))
        fprintf(stderr, "!!Interrupt missed\n");

    for (i = 0; i < 4 && s->cpu[i]; i++) {
        old = s->b_irq[i];
        if (!(old & (1 << 2))) {
            s->b_irq[i] |= (1 << 2);
            cpu_alpha_update_irq(s->cpu[i], s->b_irq[i]);
        }
    }
}


static int typhoon_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

static void typhoon_set_irq(qemu_irq *pic, int irq_num, int level)
{
    /* Never called (yet).  */
    fprintf(stderr, "typhoon_set_irq: irq_num=%d, level=%d\n", irq_num, level);
    exit(4);
}

TyphoonState *typhoon_21272_init (uint64_t *arr, qemu_irq **irqs,
                                  qemu_irq *intim_irq, void *cpu0)
{
    TyphoonState *s;
    int cchip;
    int dchip;
    int pchip;
    int pci_io;
    int pci_cfg;
    int pci_iack;
    int i;

    s = qemu_mallocz(sizeof(TyphoonState));

    /* Cchip registers.  */
    cchip = cpu_register_io_memory(0, typhoon_cchip_read,
                                   typhoon_cchip_write, s);
    cpu_register_physical_memory(0x801a0000000ULL, 0x0002000, cchip);

    /* Dchip registers.  */
    dchip = cpu_register_io_memory(0, typhoon_dchip_read,
                                   typhoon_dchip_write, s);
    cpu_register_physical_memory(0x801b0000000ULL, 0x0002000, dchip);

    /* Pchip0 registers.  */
    pchip = cpu_register_io_memory(0, typhoon_pchip_read,
                                   typhoon_pchip_write, &s->pchip[0]);
    cpu_register_physical_memory(0x80180000000ULL, 0x0002000, pchip);

    /* Pchip1 registers.  */
    pchip = cpu_register_io_memory(0, typhoon_pchip_read,
                                    typhoon_pchip_write, &s->pchip[1]);
    cpu_register_physical_memory(0x80380000000ULL, 0x0002000, pchip);

    /* Pchip0 PCI I/O  */
    pci_io = cpu_register_io_memory(0, pchip_pci_io_read,
                                    pchip_pci_io_write, &s->pchip[0]);
    cpu_register_physical_memory(0x801fc000000ULL, 0x0010000, pci_io);

    /* Pchip0 PCI cfg  */
    pci_cfg = cpu_register_io_memory(0, pchip_pci_cfg_read,
                                    pchip_pci_cfg_write, &s->pchip[0]);
    cpu_register_physical_memory(0x801fe000000ULL, 0x0010000, pci_cfg);

    /* Pchip0 PCI IntAck  */
    pci_iack = cpu_register_io_memory(0, pchip_pci_iack_read,
                                      pchip_pci_iack_write, &s->pchip[0]);
    cpu_register_physical_memory(0x801f8000000ULL, 0x002000, pci_iack);

    /* Pchip0 PCI cfg  */
    pci_cfg = cpu_register_io_memory(0, pchip_pci_cfg_read,
                                    pchip_pci_cfg_write, &s->pchip[1]);
    cpu_register_physical_memory(0x803fe000000ULL, 0x0010000, pci_cfg);

    s->pchip[0].pci = pci_register_bus(typhoon_set_irq,
                                       typhoon_map_irq,
                                       (void *)&s->pchip[0], 0, 64);

    pci_mem_base = 0x80000000000ULL;
    isa_mem_base = 0x80000000000ULL;

    typhoon_reset(s);

    /* P1 chip present, 8 dchips, 2 memory buses.  */
    s->csc = (1 << 14) | (3 << 0);

    /* Configure cchip array address.  */
    for (i = 0; i < 4; i++)
        s->aar[i] = arr[i];

    s->cpu[0] = cpu0;
    s->irqs = qemu_allocate_irqs(cchip_set_irq, s, 64);
    *irqs = s->irqs;

    s->intim_irq = qemu_allocate_irqs(intim_set_irq, s, 1);
    *intim_irq = s->intim_irq[0];

    qemu_register_reset(&typhoon_reset, s);

    return s;
}

void typhoon_set_iack_handler(TyphoonState *c, int num,
                              int (*handler)(void *), void *param)
{
    c->pchip[num].iack_handler = handler;
    c->pchip[num].iack_handler_param = param;
}

PCIBus *typhoon_get_pci_bus(TyphoonState *c, int num)
{
    return c->pchip[num].pci;
}
