/*
 * QEMU Ali 1543c emulation
 *
 * Copyright (c) 2009 AdaCore
 *
 * Written by Tristan Gingold.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 */
#include <time.h>
#include <sys/time.h>
#include "hw.h"
#include "net.h"
#include "sysemu.h"
#include "devices.h"
#include "boards.h"
#include "pc.h"
#include "isa.h"
#include "qemu-char.h"
#include "pci.h"
#include "fdc.h"

/* Ali 1543 is a south-bridge super-IO.  It contains a DMA, a PIC, a PIT,
  a ps/2 kbd interface, 2 IDE controller, 1 USB controller (OHCI), 1 FDC,
  2 serials ports, 1 parallel port and a PMU.  */

//#define DEBUG_CFG

#define MAX_IDE_BUS 2

struct ALI1543State {
    PCIDevice pci;

    qemu_irq *irq; /* Upstream handler. */
    PITState *pit;
    qemu_irq *i8259;
    fdctrl_t *fdc;

    /* Configuration.  */
    enum cfg_state { CFG_SNOOP, CFG_51, CFG_EN } cfg_state;
    unsigned char cfg_index;
};

static void ali_cfg_write(void *opaque, uint32_t addr, uint32_t val)
{
    ALI1543State *ali = opaque;

    if ((addr & 1) == 0) {
        switch (ali->cfg_state) {
        case CFG_SNOOP:
            if (val == 0x51)
                ali->cfg_state = CFG_51;
            else
                qemu_log("ali1543-cfg: write %02x to cfg_port\n", val);
            break;
        case CFG_51:
            if (val == 0x23)
                ali->cfg_state = CFG_EN;
            else {
                qemu_log("ali1543-cfg: write %02x to cfg_port (51)\n", val);
                ali->cfg_state = CFG_SNOOP;
            }
            break;
        case CFG_EN:
            ali->cfg_index = val;
            break;
        }
    }
    else {
#ifdef DEBUG_CFG
        qemu_log("ali1543-cfg: write %02x to cfg reg %02x (addr=%x)\n",
                 val, ali->cfg_index, addr);
#endif
    }
}

static uint32_t ali_cfg_read(void *opaque, uint32_t addr)
{
    ALI1543State *ali = opaque;
    uint32_t res;

    if ((addr & 1) == 0) {
        if (ali->cfg_state != CFG_EN) {
            qemu_log("ali1543-cfg: read fromcfg_port\n");
            res = 0;
        }
        else
            res = ali->cfg_index;
    }
    else {
        switch (ali->cfg_index) {
        case 0x20:
            res = 0x43;
            break;
        case 0x21:
            res = 0x15;
            break;
        default:
#ifdef DEBUG_CFG
            qemu_log("ali1543-cfg: read from reg %02x\n", ali->cfg_index);
#endif
            res = 0;
            break;
        }
    }
    return res;
}

ALI1543State *ali1543_init (PCIBus *bus, int devfn, qemu_irq irq)
{
    ALI1543State *ali;
    uint8_t *pci_conf;
    int i;
    BlockDriverState *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    BlockDriverState *fd[MAX_FD];

    ali = (ALI1543State*)pci_register_device(bus, "Ali1543",
                                             sizeof(ALI1543State),
                                             devfn, NULL, NULL);

    pci_conf = ali->pci.config;

    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_AL);
    pci_config_set_device_id(pci_conf, PCI_DEVICE_ID_AL_M1533);
    pci_conf[0x08] = 0xc3; // Revision
    pci_config_set_class(pci_conf, PCI_CLASS_BRIDGE_ISA);
    pci_conf[0x2c] = 0; // Subsystem
    pci_conf[0x2d] = 0;
    pci_conf[0x2e] = 0;
    pci_conf[0x2f] = 0;

    register_ioport_read(0x370, 2, 1, ali_cfg_read, ali);
    register_ioport_write(0x370, 2, 1, ali_cfg_write, ali);

    ali->i8259 = i8259_init(irq);

    /* serial_init already handles NULL CharDriverState but this code adds
       a more useful label.  */
    if (serial_hds[0] == NULL)
        serial_hds[0] = qemu_chr_open("com1", "nul", NULL);
    if (serial_hds[1] == NULL)
        serial_hds[1] = qemu_chr_open("com2", "nul", NULL);

    serial_init(0x3f8, ali->i8259[4], 115200, serial_hds[0]);
    serial_init(0x2f8, ali->i8259[3], 115200, serial_hds[1]);
    ali->pit = pit_init(0x40, ali->i8259[0]);
    pcspk_init(ali->pit);

    DMA_init(1);
    for(i = 0; i < MAX_FD; i++) {
        int index;

        index = drive_get_index(IF_FLOPPY, 0, i);
        if (index != -1)
            fd[i] = drives_table[index].bdrv;
        else
            fd[i] = NULL;
    }
    ali->fdc = fdctrl_init(ali->i8259[6], 2, 0, 0x3f0, fd);

    for(i = 0; i < MAX_IDE_BUS * MAX_IDE_DEVS; i++) {
        int index;
        index = drive_get_index(IF_IDE, i / MAX_IDE_DEVS, i % MAX_IDE_DEVS);
        if (index != -1)
            hd[i] = drives_table[index].bdrv;
        else
            hd[i] = NULL;
    }
    pci_m5229_ide_init(bus, hd, devfn + (8 << 3), ali->i8259);

    return ali;
}

qemu_irq ali1543_get_irq(ALI1543State *c, int n)
{
    return c->i8259[n];
}
