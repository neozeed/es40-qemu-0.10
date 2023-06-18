/*
 * QEMU ES-40 System Emulator
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
#include "pci.h"
#include "block.h"
#include "flash.h"

/* Building the firmware for es40:

   The main part of the firmware is called SRM.  You can get an original
   image from HP web site:

   ftp://ftp.hp.com/pub/alphaserver/firmware/current_platforms/v7.3_release/ES40_series/ES40/es40.zip

   Extract the archive and copy cl67srmrom.exe into a directory.

   Then start Qemu using es40-rombuild machine:

   $ qemu-system-alpha -M es40-rombuild -boot n -L .

   This builds the rom, writes es40.rom file and exits.  Now you have it.
*/

//#define DEBUG_TIGBUS

/* For Rombuild.  */
#define ROMBUILD_BIOS_FILENAME "cl67srmrom.exe"
#define LFU_HDR_SIZE 0x240
#define LFU_START_ADDR 0x900000
#define LFU_LOAD_ADDR  0x900000
#define LFU_MEM_SIZE   0x1000000
#define LFU_RAM_OFFSET 0x400000

#define SRM_SIZE 0x200000

/* For es40.  */
#define BIOS_FILENAME "es40.rom"

#define VGABIOS_CIRRUS_FILENAME "vgabios-cirrus.bin"

static ram_addr_t ram_offset;

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

typedef struct TigBusState TigBusState;
struct TigBusState {
    /* See 264dptrm.pdf.  */
    unsigned char halt_a;
    unsigned char halt_b;
};

static uint32_t tigbus_readl (void *opaque, target_phys_addr_t addr)
{
    TigBusState *s = opaque;
    uint32_t ret;
    int reg = addr >> 6;

    /* TIGbus registers are mostly only 8 bits.  */
    if (addr & 0x04)
        return 0;

    switch (reg) {
    case 0x17:
        ret = s->halt_b;
        break;
    case 0x12: /* Power fault detected.  */
        ret = 0;
        break;
    case 0x04:
        ret = 0;
        break;
    default:
        qemu_log("tigbus: unhandled read reg=%02x\n", reg);
        ret = 0;
        break;
    }

#ifdef DEBUG_TIGBUS
    fprintf(stderr,"tigbus read  reg=%02x, val=%08x\n", reg, ret);
#endif
    return ret;
}

static void tigbus_writel (void *opaque,
                            target_phys_addr_t addr, uint32_t value)
{
    TigBusState *s = opaque;
    int reg = addr >> 6;

    if (addr & 0x04)
        return;

#ifdef DEBUG_TIGBUS
    fprintf(stderr,"tigbus write reg=%02x, val=%08x\n", reg, value);
#endif

    switch (reg) {
    case 0x17:
        s->halt_b = value & 0x0f;
        break;
    case 0x12: /* Power fault detected.  */
        break;
    default:
        qemu_log("tigbus: unhandled write to reg=0x%02x (%08x)\n",
                 reg, value);
        break;
    }
}

static CPUReadMemoryFunc *tigbus_read[] = {
    &illegal_read,
    &illegal_read,
    &tigbus_readl,
};

static CPUWriteMemoryFunc *tigbus_write[] = {
    &illegal_write,
    &illegal_write,
    &tigbus_writel,
};

/* Flash content (sectors):
   TIG:   0
   SRM:   1-14
   EEROM: 15
   SROM:  16-17
   ARC:   18-30
   ARC variables: 31 */
static uint32_t flash_readl(void *opaque, target_phys_addr_t addr)
{
    /* Flash registers are mostly only 8 bits.  */
    if (addr & 0x04)
        return 0;

    return am29f016_readb((Am29f016State *)opaque, addr >> 6);
}

static void flash_writel (void *opaque,
                          target_phys_addr_t addr, uint32_t value)
{
    if (addr & 0x04)
        return;

    am29f016_writeb((Am29f016State *)opaque, addr >> 6, value);
}

static CPUReadMemoryFunc *flash_read[] = {
    &illegal_read,
    &illegal_read,
    &flash_readl,
};

static CPUWriteMemoryFunc *flash_write[] = {
    &illegal_write,
    &illegal_write,
    &flash_writel,
};

/* Dual Port Ram.

0 0 SROM EV6 BIST status 1=good  0=bad
1 1 SROM Bit[7]=Master Bits[0,1]=CPU_ID
2 2 SROM Test STR status 1=good  0=bad
3 3 SROM Test CSC status 1=good  0=bad
4 4 SROM Test Pchip 0 PCTL status 1=good, 0=bad
5 5 SROM Test Pchip 1  PCTL status 1=good, 0=bad
6 6 SROM Test DIMx status 1=good  0=bad
7 7 SROM Test TIG bus status
8 8 SROM Dual-Port RAM test DD= started
9 9 SROM Status of DPR test 1=good  0=bad
A A SROM Status of CPU speed function  FF=good, 0=bad
B B SROM Lower byte of CPU speed in MHz
C C SROM Upper byte of CPU speed in MHz
D:F - - Reserved
10:15 SROM Power On Time Stamp for CPU 0—written as BCD
 Byte 10 = Hours (0-23)
 Byte 11 = Minutes (0-59)
 Byte 12 = Seconds (0-59)
 Byte 13 = Day of Month (1-31)
 Byte 14 = Month (1-12)
 Byte 15 = Year (0-99)
16 SROM SROM Power On Error Indication for CPU is "alive."
 For example; 0 = no error, 2 = Secondary time-out Error, 3 = Bcache Error
17:1D Unused
1E SROM Last "sync state" reached;  80=Finished GOOD
1F SROM Size of Bcache in MB
20:3F 20 Repeat  for CPU1 of CPU0    0-1F
40:5F 20 Repeat  for CPU2 of CPU0    0-1F
60:7F 20 Repeat  for CPU3 of CPU0    0-1F
80 80 SROM Array 0 (AAR 0)  Configuration
   Bits<7:4>
 4 = non split -  lower set only
 5 = split - lower set only
 9 = split -  upper set only
 D = split - 8 DIMMs
 F = Twice split -  8 DIMMs
     Bits<3:0>
 0 = Configured -  Lowest array
 1 = Configured -  Next lowest array
 2 = Configured -  Second highest  array
 3 = Configured -  Highest array
 4 = Misconfigured -  Missing DIMM(s)
 8 = Miconfigured -  Illegal DIMM(s)
 C = Misconfigured -  Incompatible  DIMM(s)
81 81 SROM Array 0 (AAR 0)Size (x64 Mbytes)
  0 = no good memory
  1 =   64 Mbyte
  2 = 128 Mbyte
  4 = 256 Mbyte
  8 = 512 Mbyte
 10 =     1 Gbyte
 20 =     2 Gbyte
 40 =     4 Gbyte
 80 =     8 Gbyte
82 82 SROM Array 1 (AAR 1) Configuration
83 83 SROM Array 1 (AAR 1) Size (x64 Mbytes)
84 84 SROM Array 2  (AAR 2) Configuration
85 85 SROM Array 2 (AAR  2) Size (x64 Mbytes)
86 86 SROM Array 3  (AAR 3) Configuration
87 87 SROM Array 3 (AAR 3) Size (x64 Mbytes)
88:8B SROM Byte to define failed DIMMs for MMBs
88 - MMB 0
89 - MMB 1
8A - MMB 2
8B - MMB 3
 Bit set indicates failure.
 Bit definitions ( bit 0 = DIMM 1, bit 1 = DIMM2,
 bit 2 = DIMM 3, bit 7 = DIMM 8)
8C:8F 8C-8F SROM Byte to define misconfigured DIMMs for MMBs
8C - MMB 0
8D - MMB 1
8E - MMB 2
8F - MMB 3
 Bit definitions ( bit 0 = DIMM 1, bit 1 = DIMM2,
 bit 2 = DIMM 3, bit 7 = DIMM 8)
90 90 RMC Power Supply/VTERM  present
91 91 RMC Power Supply  PS_POK bits
92 92 RMC AC input value from Power Supply
93:96 93 RMC Temperature from CPU(x) in BCD
97:99 97 RMC Temperature Zone(x) from 3 PCI temp sensors
9A:9F 9A RMC Fan Status; Raw Fan speed value
A0:A9 A0 RMC Failure registers used as part of the 680 machine
 check logout frame.  See Appendix D.
AA RMC Fan status (bit 0 = fan 1, bit 1 = fan 2,
 1- indicates good; 0 indicates fan failure
AB RMC Status of RMC to read I2C bus of MMB0  DIMMs
 Definition:
   Bit 7 - DIMM 8   0=OK  1=Fail
   Bit 6 - DIMM 7
   Bit 5 - DIMM 6
    ...
   Bit 0 - DIMM 1
AC RMC Status of RMC to read I2C bus of MMB1 DIMMs
AD RMC Status of RMC to read I2C bus of MMB2 DIMMs
AE RMC Status of RMC to read I2C bus of MMB3 DIMMs
AF RMC Status of RMC to read MMB and CPU I2C buses
 Definition:
  Bit 7 - MMB3     0=OK  1=Fail
  Bit 6 - MMB2
  Bit 5 - MMB1
  Bit 4 - MMB0
  Bit 3 - CPU3
  Bit 2 - CPU2
  Bit 1 - CPU1
  Bit 0 - CPU0
B0 RMC Status of RMC to read CPB (PCI backplane) I2C EEROM 0=OK  1 = fail
B1 RMC Status of RMC to read CSB (motherboard) I2C EEROM  0=OK  1 = fail
B2 RMC Status of RMC to read SCSI backplane
Definition:
  Bit 0 - SCSI backplane 0
  Bit 1 - SCSI backplane 1
  Bit 4 - Power supply 0
  Bit 5 - Power supply 1
  Bit 6 - Power supply 2
B3:B9 Unused Unused
BA RMC I2C done, BA =  finished
BB RMC RMC Power on Error indicates error during power-up (1=Flash Corrupted)
BC RMC RMC flash update error status
BD RMC Copy of PS input Value.  See Appendix D.
BE RMC Copy of the byte from the I/O expanders on the
 SPC loaded by the RMC on fatal errors.  See Appendix D.
BF RMC Reason for system failure.  See Appendix D.
C0:D8 Unused
D9 RMC Baud rate
DA TIG Indicates TIG finished loading its code (0xAA indicates done)
DB:E3 RMC Fan/Temp info from PS1
E4:EC RMC Fan/Temp info from PS2
ED:F5 RMC Fan/Temp info from PS3
F6:F8 Unused Unused
F9 Firmware Buffer Size (0-0xFF) or 1 to 256 bytes
FA:FB FA Firmware Command address qualifier
FA = lower byte, FB = upper byte
FC FC RMC Command status associated with the RMC
 response to a request from the firmware
  0 = successful completion
80 = unsuccessful completion
81 = invalid command code
82 = invalid command qualifier
FD FD RMC Command ID associated with the RMC
  response to a request from the firmware
FE FE Firmware Command Code associated with a “command”
sent to the RMC
  1 = update I2C EEROM
  2 = update baud rate
  3 = display to OCP
F0 = update RMC flash
FF FF Firmware Command ID associated with a “command” sent to the RMC
100:1FF 100 RMC Copy of EEROM on MMB0 J1 DIMM 1,
 initially read on I2C bus by RMC when 5
 volts supply turned on. Written by Compaq
 Analyze after error diagnosed to particular FRU
200:2FF 200 RMC Copy of EEROM on MMB0 J2 DIMM 2
300:3FF 300 RMC Copy of EEROM on MMB0 J3 DIMM 3
400:4FF 400 RMC Copy of EEROM on MMB0 J4 DIMM 4
500:5FF 500 RMC Copy of EEROM on MMB0 J5 DIMM 5
600:7FF 600 RMC Copy of EEROM on MMB0 J6 DIMM 6
700:7FF 700 RMC Copy of EEROM on MMB0 J7 DIMM 7
800:8FF 800 RMC Copy of EEROM on MMB0 J8 DIMM 8
900:9FF 900 RMC Copy of EEROM on MMB1 J1 DIMM 1
A00:AFF A00 RMC Copy of EEROM on MMB1 J2 DIMM 2
B00:BFF B00 RMC Copy of EEROM on MMB1 J3 DIMM 3
C00:CFF C00 RMC Copy of EEROM on MMB1 J4 DIMM 4
D00:DFF D00 RMC Copy of EEROM on MMB1 J5 DIMM 5
E00:EFF E00 RMC Copy of EEROM on MMB1 J6 DIMM 6
F00:FFF F00 RMC Copy of EEROM on MMB1 J7 DIMM 7
1000:10FF 1000 RMC Copy of EEROM on MMB1 J8 DIMM 8
1100:11FF 1100 RMC Copy of EEROM on MMB2 J1 DIMM 1
1200:12FF 1200 RMC Copy of EEROM on MMB2 J2 DIMM 2
1300:13FF 1300 RMC Copy of EEROM on MMB2 J3 DIMM 3
1400:14FF 1400 RMC Copy of EEROM on MMB2 J4 DIMM 4
1500:15FF 1500 RMC Copy of EEROM on MMB2 J5 DIMM 5
1600:16FF 1600 RMC Copy of EEROM on MMB2 J6 DIMM 6
1700:17FF 1700 RMC Copy of EEROM on MMB2 J7 DIMM 7
1800:18FF 1800 RMC Copy of EEROM on MMB2 J8 DIMM 8
1900:19FF 1900 RMC Copy of EEROM on MMB3 J1 DIMM 1
1A00:1AFF 1A00 RMC Copy of EEROM on MMB3 J2 DIMM 2
1B00:1BFF 1B00 RMC Copy of EEROM on MMB3 J3 DIMM 3
1C00:1CFF 1C00 RMC Copy of EEROM on MMB3 J4 DIMM 4
1D00:1DFF 1D00 RMC Copy of EEROM on MMB3 J5 DIMM 5
1E00:1EFF 1E00 RMC Copy of EEROM on MMB3 J6 DIMM 6
1F00:1FFF 1F00 RMC Copy of EEROM on MMB3 J7 DIMM 7
2000:20FF 2000 RMC Copy of EEROM on MMB3 J8 DIMM 8
2100:21FF 2100 RMC Copy of EEROM from CPU0
2200:22FF 2200 RMC Copy of EEROM from CPU1
2300:23FF 2300 RMC Copy of EEROM from CPU2
2400:24FF 2400 RMC Copy of EEROM from CPU3
2500:25FF 2500 RMC Copy of MMB 0  J5 FRU EEROM
2600:26FF 2600 RMC Copy of MMB 1  J7 FRU EEROM
2700:27FF 2700 RMC Copy of MMB 2  J6 FRU EEROM
2800:28FF 2800 RMC Copy of MMB 3  J8 FRU EEROM
2900:29FF 2900 RMC Copy of EEROM on CPB (PCI backplane)
2A00:2AFF 2A00 RMC Copy of EEROM on CSB (motherboard)
2B00:2BFF 2B00 RMC Last EV6 Correctable Error-ASCII
 character string that indicates correctable
 error occurred, type, FRU, and so on.  Backed
 up in CSB (motherboard) EEROM. Written by Compaq Analyze
2C00:2CFF 2C00 RMC Last Redundant Failure-ASCII
 character string that indicates redundant
 failure occurred, type, FRU, and so on.
 Backed up in system CSB (motherboard) EEROM. Written by Compaq Analyze
2D00:2DFF 2D00 RMC Last System Failure-ASCII character
 string that indicates system failure occurred, type, FRU, and so on. Backed
 up in CSB (motherboard) EEROM. Written by Compaq Analyze.
2E00:2FFF 2E00 RMC Uncorrectable machine logout frame (512 bytes)
3000:3008 SROM SROM Version  (ASCII string)
3009:300B RMC Rev Level of RMC first byte is letter Rev
 [x/t/v] second 2 bytes are major/minor.
 This is the rev level of the RMC on-chip code.
300C:300E RMC Rev Level of RMC first byte is letter Rev
 [x/t/v] second 2 bytes are major/minor.
 This is the rev level of the RMC flash code.
300F:3010 300F RMC Revision Field of the DPR Structure
3011:30FF Unused Unused
3100:31FF RMC Copy of PS0 EEROM (first 256 bytes)
3200:32FF RMC Copy of PS1  EEROM (first 256 bytes)
3300:33FF RMC Copy of PS2  EEROM (first 256 bytes)
3400 SROM Size of Bcache in MB
3401 SROM Flash SROM is valid flag; 8 = valid, 0 = invalid
3402 SROM System's errors determined by SROM
3403:340F SROM/SRM Reserved for future SROM/SRM communication
3410:3417 SROM/SRM Jump to address for CPU0
3418 SROM/SRM Waiting to jump to flag for CPU0
3419 SROM Shadow of value written to EV6 DC_CTL register.
341A:341E SROM Shadow of most recent writes to EV6 CBOX 'Write-many' chain.
341F SROM/SRM Reserved for future SROM/SRM communication
3420:342F SROM/SRM Repeat  for CPU1 of CPU0    3410-341F
3430:343F SROM/SRM Repeat  for CPU2 of CPU0    3410-341F
3440:344F SROM/SRM Repeat  for CPU3 of CPU0    3410-341F
3450:349F SROM/RMC Reserved for SROM mini-console via
 RMC communication area. Future design.
34A0:34A7 SROM Array 0 to DIMM ID translation
 Bits<7:5>
 0 = Exists, No Error
 1 = Expected Missing
 2 = Error - Missing  DIMM(s)
 4 = Error - Illegal DIMM(s)
 6 = Error - Incompatible DIMM(s)
 Bits<4:0>
 Bits <2:0> = DIMM + 1 (1-8)
 Bits <4:3> = MMB (0-3)
34A8:34AF SROM Repeat for Array 1 of Array 0
34A0:34A7
34B0:34B7 SROM Repeat for Array 2 of Array 0
34A0:34A7
34B8:34CF SROM Repeat for Array 3 of Array 0
34A0:34A7
34C0:34FF 34C0 SROM Used as scratch area for SROM
3500:35FF Firmware Used as the dedicated buffer in which
 SRM writes OCP or FRU EEROM data.
 Firmware will write this data, RMC will only read this data.
3600:36FF 3600 SRM Reserved
3700:37FF SRM Reserved
3800:3AFF RMC RMC scratch space
3B00:3BFF RMC First SCSI backplane EEROM
3C00:3CFF RMC Second SCSI backplane EEROM
3D00:3DFF RMC PS0 second 256 bytes
3E00:3EFF RMC PS1 second 256 bytes
3F00:3FFF RMC PS2 second 256 bytes
*/

struct DPRamState {
    unsigned char mem[0x4000];
    uint32_t set_vec[0x4000 >> 5];
};
typedef struct DPRamState DPRamState;

static uint32_t tigbus_dpram_readl (void *opaque, target_phys_addr_t addr)
{
    DPRamState *dpr = opaque;
    int reg = addr >> 6;

    /* Discard upper long word.  */
    if (addr & 0x4)
        return 0;

#if 0
    if (!(dpr->set_vec[reg >> 5] &(1 << (reg & 0x1f))))
        fprintf(stderr,"tigbus dpram: reading uninitialized 0x%04x\n", reg);
#endif

    return dpr->mem[reg];
}

static void tigbus_dpram_writel (void *opaque,
                               target_phys_addr_t addr, uint32_t value)
{
    DPRamState *dpr = opaque;
    int reg = addr >> 6;

    /* Discard upper long word.  */
    if (addr & 0x4)
        return;

#if 0
    fprintf(stderr, "tigbus dpram write %02x at %08x (%04x)\n",
            value, (unsigned int) addr, reg);
#endif

    dpr->mem[reg] = value;
    dpr->set_vec[reg >> 5] |= (1 << (reg & 0x1f));
    if (reg == 0xff) {
        const char *cmd;
        dpr->mem[0xfc] = 0;
        dpr->mem[0xfd] = dpr->mem[0xff];
        switch (dpr->mem[0xfe]) {
        case 0x01:
            cmd = "eeprom update";
            break;
        case 0x02:
            cmd = "baud rate update";
            break;
        case 0x03:
        {
            char buf[17];
            memcpy(buf, &dpr->mem[0x3500], 16);
            buf[16] = 0;
            fprintf(stderr,"OCP message: [%s]\n", buf);
#if 0
            if (strncmp (buf, "create powerup  ", 16) == 0) {
                loglevel = CPU_LOG_TB_IN_ASM | CPU_LOG_EXEC | CPU_LOG_TB_CPU;
            }
#endif
            if (1)
              cmd = NULL;
            else
              cmd = "OCP display";
            break;
        }
        case 0xf0:
            cmd = "update RMC flash";
            break;
        default:
            cmd = NULL;
            fprintf(stderr,"tigbus dpram: cmd unknown 0x%02x\n",
                    dpr->mem[0xfe]);
            dpr->mem[0xfc] = 0x81;
            break;
        }
        if (cmd) {
            fprintf(stderr,
                    "tigbus dpram: cmd %02x, id=%02x: %s, addr=%04x len=%d\n",
                    dpr->mem[0xfe], dpr->mem[0xff],
                    cmd, (dpr->mem[0xfb] << 8) | dpr->mem[0xfa],
                    dpr->mem[0xf9] + 1);
        }
    }
}

static CPUReadMemoryFunc *tigbus_dpram_read[] = {
    &illegal_read,
    &illegal_read,
    &tigbus_dpram_readl,
};

static CPUWriteMemoryFunc *tigbus_dpram_write[] = {
    &illegal_write,
    &illegal_write,
    &tigbus_dpram_writel,
};

static void dpr_set(DPRamState *dpr, int reg, unsigned char val)
{
    dpr->mem[reg] = val;
    dpr->set_vec[reg >> 5] |= (1 << (reg & 0x1f));
}

static void tigbus_reset (void *opaque)
{
    /* TigBusState *s = opaque; */
}

static void tigbus_init (uint64_t arr[], BlockDriverState *flash_bs)
{
    TigBusState *s;
    int mem;
    int dpram_io;
    int flash_io;
    DPRamState *dpram;
    Am29f016State *flash;
    int i;

    s = qemu_mallocz(sizeof(TigBusState));
    mem = cpu_register_io_memory(0, tigbus_read, tigbus_write, s);
    cpu_register_physical_memory(0x80130000000ULL, 0x0002000, mem);

    dpram = qemu_mallocz(sizeof(DPRamState));
    dpram_io = cpu_register_io_memory(0, tigbus_dpram_read,
                                      tigbus_dpram_write, dpram);
    cpu_register_physical_memory(0x80110000000ULL, 0x0100000, dpram_io);

    /* Initialize dpram.  */
#define S(r,v) dpr_set(dpram,r,v)
    S(0x00, 1); /* BIST OK */
    S(0x01, 0x80); /* CPU 0 is master */
    S(0x02, 1); /* STR ok */
    S(0x03, 1); /* CSC ok */
    S(0x04, 1); /* Pchip0 ok */
    S(0x05, 1); /* Pchip1 ok */
    S(0x06, 1); /* DIMx ok */
    S(0x07, 1); /* TIG ok */
    S(0x08, 0xdd); /* DPRam ok */
    S(0x09, 0x1); /* DPRDDam ok */
    S(0x0a, 0xff); /* cpu speed ok */
    S(0x0b, 600 % 256);
    S(0x0c, 600 / 256);
    /* Unused.  */
    S(0x0d, 0);
    S(0x0e, 0);
    S(0x0f, 0);
    /* Power On time stamp.  Use RTC ?  */
    S(0x10, 0x12);
    S(0x11, 0x30);
    S(0x12, 0x25);
    S(0x13, 0x15);
    S(0x14, 0x02);
    S(0x15, 0x05);
    S(0x16, 0x0); /* Power On indicator.  */
    /* Unused.  */
    S(0x17, 0);
    S(0x18, 0);
    S(0x19, 0);
    S(0x1a, 0);
    S(0x1b, 0);
    S(0x1c, 0);
    S(0x1d, 0);
    S(0x1e, 0x80); /* Finished good */
    S(0x1f, 0x8); /* Bcache size */

    for (i = 0x20; i < 0x80; i++)
        S(i, 0);    /* For other cpus.  */

    /* Array configuration */
    for (i = 0x0; i < 0x4; i++) {
        S(0x80 + 2 * i, arr[i] ? 0xf0 | i : 4);
        if (arr[i]) {
            int msb = ((arr[i] >> 12) & 0x0f) - 1;
            if (msb < 2)
                msb = 2;
            S(0x80 + 2 * i + 1, 1 << (msb - 2));
        }
        else
            S(0x80 + 2 * i + 1, 0);
    }
    for (i = 0x88; i < 0x90; i++)
        S(i, 0);
    S(0x90, 0xff); /* PSU */
    S(0x91, 0x00); /* PSU */
    S(0x92, 0x07); /* AC */
    S(0x93, 0x30); /* CPU 0 */
    S(0x94, 0x00); /* CPU 1 */
    S(0x95, 0x00); /* CPU 2 */
    S(0x96, 0x00); /* CPU 3 */
    S(0x97, 0x22); /* Pci 0 */
    S(0x98, 0x22); /* Pci 1 */
    S(0x99, 0x22); /* Pci 2 */
    S(0x9a, 0x90); /* fan speed */
    S(0x9b, 0x90); /* fan speed */
    S(0x9c, 0x90); /* fan speed */
    S(0x9d, 0x90); /* fan speed */
    S(0x9e, 0x90); /* fan speed */
    S(0x9f, 0x90); /* fan speed */
    for (i = 0xa0; i < 0xaa; i++)
        S(i, 0); /* 680 logout frame */
    S(0xaa, 0); /* Fan status */
    S(0xab, 0); /* MMB0 dimm i2c */
    S(0xac, 0); /* MMB1 dimm i2c */
    S(0xad, 0); /* MMB2 dimm i2c */
    S(0xae, 0); /* MMB3 dimm i2c */
    S(0xaf, 0); /* MMB & cpu i2c */
    S(0xb0, 0); /* cpb i2c */
    S(0xb1, 0); /* csb i2c */
    S(0xb2, 0); /* scsi and ps i2c */
    for (i = 0xb3; i < 0xba; i++)
        S(i, 0); /* usued */
    S(0xba, 0xba); /* i2c done */
    S(0xbb, 0x0); /* rmc flash ok */
    S(0xbc, 0x0); /* rmc flash ok */

    S(0xbd, 0x7); /* PS val */
    S(0xbe, 0x0); /* SPC fault */
    S(0xbf, 0x0); /* system fault */

    for (i = 0xc0; i <= 0xd8; i++)
        S(i, 0); /* usued */

    S(0xd9, 2); /* RMC baud rate.  */
    S(0xda, 0xaa); /* TIG loaded.  */
    for (i = 0; i < 3; i++) {
        S(0xdb + i * 9, 0xf4 + i); /* PS id */
        S(0xdc + i * 9, 0x45);
        S(0xdd + i * 9, 0x51);
        S(0xde + i * 9, 0x37);
        S(0xdf + i * 9, 0x90); /* Fan speed */
        S(0xe0 + i * 9, 0xd6);
        S(0xe1 + i * 9, 0x49);
        S(0xe2 + i * 9, 0x4b);
        S(0xe3 + i * 9, 0x0);
    }
    S(0xf6, 0); /* unused.  */
    S(0xf7, 0); /* unused.  */
    S(0xf8, 0); /* unused.  */
    S(0xf9, 0); /* buffer size.  */
    S(0xfa, 0); /* cmd addr.  */
    S(0xfb, 0); /* cmd addr.  */
    S(0xfc, 0); /* command status.  */
    S(0xfd, 1); /* command id.  */
    S(0xff, 1); /* command id.  */

    for (i = 0x2900; i < 0x2a00; i++)
        S(i, 0);    /* PCI backplane FRU.  */
    for (i = 0x2a00; i < 0x2b00; i++)
        S(i, 0);    /* motherboard FRU.  */
    for (i = 0x2b00; i < 0x2c00; i++)
        S(i, 0);    /* last correctable error.  */
    for (i = 0x2c00; i < 0x2d00; i++)
        S(i, 0);    /* last redundant error.  */
    for (i = 0x2d00; i < 0x2e00; i++)
        S(i, 0);    /* last system failure.  */

    S(0x3000, 'V'); /* SROM version  */
    S(0x3001, '2');
    S(0x3002, '.');
    S(0x3003, '2');
    S(0x3004, '2');
    S(0x3005, 'G');
    S(0x3006, 0);
    S(0x3007, 0);
    S(0x3008, 0);
    S(0x3009, 'V'); /* RMC rom version */
    S(0x300a, '1');
    S(0x300b, '0');
    S(0x300c, 'V'); /* RMC flash version */
    S(0x300d, '1');
    S(0x300e, '0');
    S(0x300f, '1'); /* RMC revision field */
    S(0x3010, '0');
    for (i = 0x3011; i <= 0x30ff; i++)
        S(i, 0);    /* unsued */

    S(0x3400, 8); /* Size of Bcache */
    S(0x3401, 8); /* Flash rom valid */
    S(0x3402, 0); /* system errors */
    for (i = 0x3403; i < 0x3410; i++)
        S(i, 0);    /* Reserved.  */
    for (i = 0x3410; i < 0x3420; i++)
        S(i, 0);    /* Jump to address.  */
    for (i = 0x3420; i <= 0x349f; i++)
        S(i, 0);    /* cpu 1-3 */
    /* Array map.  */
    for (i = 0; i < 0x20; i++) {
        if (arr[i / 8])
            S(0x34a0 + i, (((i >> 0) & 1) | (((i >> 1) & 1) << 3)
                           | (((i >> 2) & 1) << 2) | (((i >> 3) & 1) << 1)
                           | (((i >> 4) & 1) << 4)));
        else
            S(0x34a0 + i, 0x20);
    }
#undef S

    flash = am29f016_init(flash_bs);

    flash_io = cpu_register_io_memory(0, flash_read, flash_write, flash);
    cpu_register_physical_memory(0x80100000000ULL, 0x8000000, flash_io);

    tigbus_reset(s);
    qemu_register_reset(&tigbus_reset, s);
}

struct srm_patch {
  uint64_t addr;
  uint32_t old_insn;
  uint32_t new_insn;
};

static const struct srm_patch srm_patches[] = {
  { 0x142a0, 0x259f11e2, 0x259f0000 }, /* ldah s3, 0x11e2 */
  { 0x142a4, 0x218ca300, 0x218c0001 }, /* lda s3,-23808(s3) */
  { 0x14260, 0x259f00e5, 0x259f0000 }, /* ldah s3,229 */
  { 0x14264, 0x218ce1c0, 0x218c0001 }, /* lda s3,-7744(s3) */

#if 1
  /* Disable memory testing.  */
  { 0x8bb88, 0xe4200004, 0xc3e00004 }, /* beq t0,0x8bb9c -- aa */
  { 0x8bbc0, 0xe400001b, 0xc3e0001b }, /* beq v0,0x8bc30 -- 55 */
  { 0x8bc48, 0xe4a0001b, 0xc3e0001b }, /* beq t4,0x8bcb8 -- 00 */
#endif
  { 0, 0, 0}
};

void alpha_21264_srm_write(CPUState *env)
{
    int fd;
    int i;
    uint32_t insn;
    static const char rom_file[] = BIOS_FILENAME;

    env->a21264.pal_reloc_val = 1;
    env->a21264.pal_reloc_mask = 0;
    env->a21264.pal_reloc_offset = 0;

    /* Apply patch list.  */
    for (i = 0; srm_patches[i].addr; i++) {
        insn = ldl_phys(srm_patches[i].addr);
        if (insn != srm_patches[i].old_insn)
            fprintf(stderr, "SRM patch mismatch at %"PRIx64": insn=%x\n",
                    srm_patches[i].addr, insn);
        else
            stl_phys(srm_patches[i].addr, srm_patches[i].new_insn);
    }

    /* Save SRM.  */
    fd = open(rom_file, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0644);
    if (fd < 0) {
      save_error:
        fprintf(stderr,"qemu: can't open %s\n", rom_file);
        exit(1);
    }
    if (write(fd, phys_ram_base + ram_offset, SRM_SIZE) != SRM_SIZE)
        goto save_error;
    close(fd);
    printf ("Bios written to %s\nExit\n", rom_file);
    exit (0);
}

static void es40_rombuild_init(ram_addr_t ram_size, int vga_ram_size,
                               const char *boot_device,
                               const char *kernel_filename,
                               const char *kernel_cmdline,
                               const char *initrd_filename,
                               const char *cpu_model)
{
    CPUState *env;
    int bios_size;
    int srm_ram_size;
    int fd;
    char buf[1024];

    if (!cpu_model)
        cpu_model = "21264";

    printf("Initializing CPU\n");
    env = cpu_init(cpu_model);
    if (!env) {
        fprintf(stderr, "Unable to find Alpha CPU definition\n");
        exit(1);
    }

    /* BIOS load */
    if (bios_name == NULL)
        bios_name = ROMBUILD_BIOS_FILENAME;
    snprintf(buf, sizeof(buf), "%s/%s", bios_dir, bios_name);
    fd = open(buf, O_RDONLY | O_BINARY);
    if (fd < 0) {
      load_error:
        fprintf(stderr,"qemu: can't open %s\n", buf);
        exit(1);
    }
    bios_size = lseek(fd, 0, SEEK_END);
    if (bios_size <= 0)
        goto load_error;
    bios_size -= LFU_HDR_SIZE;
    srm_ram_size = TARGET_PAGE_ALIGN(bios_size);
    ram_offset = qemu_ram_alloc(LFU_MEM_SIZE);
    lseek(fd, LFU_HDR_SIZE, SEEK_SET);
    if (read(fd, phys_ram_base + ram_offset + LFU_LOAD_ADDR, bios_size)
        != bios_size)
        goto load_error;
    close(fd);
    memcpy(phys_ram_base + ram_offset + LFU_LOAD_ADDR + LFU_RAM_OFFSET,
           phys_ram_base + ram_offset + LFU_LOAD_ADDR,
           bios_size);
    printf ("SRM loaded\n");

    cpu_register_physical_memory(0, LFU_MEM_SIZE, ram_offset);

    env->pc = LFU_START_ADDR;
    env->a21264.pal_reloc_val = LFU_LOAD_ADDR;
    env->a21264.pal_reloc_mask = ~(uint64_t)0xfffffU;
    env->a21264.pal_reloc_offset = LFU_RAM_OFFSET;
}

static void es40_cpu_reset(void *opaque)
{
    CPUState *env = opaque;

    env->pc = 0x8000;
}

static void configure_mem_array(ram_addr_t ram_size, uint64_t *aar)
{
    int msb;
    uint32_t base;
    int i;
    uint32_t size;

    for (i = 0; i < 4; i++)
        aar[i] = 0;

    /* Configure cchip array address.  */
    size = ram_size >> 24;
    base = 0;
    for (i = 0; i < 4; i++) {
        msb = ffs(size);
        if (msb == 0)
            break;
        msb--;
        if (msb > 8)
            msb = 8;
        aar[i] = (((uint64_t)base) << 24)
            | ((msb + 1) << 12) | (1 << 2) | (1 << 0);
        base += 1 << msb;
        size -= 1 << msb;
        printf ("es40: arr[%d]=%016"PRIx64": %4uMB at %5uMB\n",
                i, aar[i], 16 << msb, base << 4);
    }
}

static void es40_init(ram_addr_t ram_size, int vga_ram_size,
                      const char *boot_device,
                      const char *kernel_filename, const char *kernel_cmdline,
                      const char *initrd_filename, const char *cpu_model)
{
    CPUState *env;
    char buf[1024];
    qemu_irq *cchip_irqs;
    qemu_irq tim_irq;
    TyphoonState *typhoon;
    ALI1543State *ali;
    PCIBus *hose0;
    uint64_t arr[4];
    BlockDriverState *flash_bs = NULL;
    int index;
    ram_addr_t vga_ram_addr;
    RTCState *rtc;

    if (!cpu_model)
        cpu_model = "21264";

    env = cpu_init(cpu_model);
    if (!env) {
        fprintf(stderr, "Unable to find Alpha CPU definition\n");
        exit(1);
    }
    qemu_register_reset(es40_cpu_reset, env);

    /* Allocate RAM.  */
    ram_offset = qemu_ram_alloc(ram_size);
    cpu_register_physical_memory(0, ram_size, ram_offset);

    /* allocate VGA RAM */
    vga_ram_addr = qemu_ram_alloc(vga_ram_size);

    index = drive_get_index(IF_PFLASH, 0, 0);
    if (index != -1)
        flash_bs = drives_table[index].bdrv;
    else
        flash_bs = NULL;

    /* SRM load */
    if (bios_name == NULL)
        bios_name = BIOS_FILENAME;
    snprintf(buf, sizeof(buf), "%s/%s", bios_dir, bios_name);
    if (load_image(buf, phys_ram_base + ram_offset) != SRM_SIZE) {
        fprintf(stderr,"qemu: can't read %s - (or bad size)\n", buf);
        exit(1);
    }

    configure_mem_array(ram_size, arr);
    typhoon = typhoon_21272_init(arr, &cchip_irqs, &tim_irq, env);
    tigbus_init(arr, flash_bs);

    hose0 = typhoon_get_pci_bus(typhoon, 0);

    ali = ali1543_init(hose0, PCI_DEVFN(7,0), cchip_irqs[55]);

    typhoon_set_iack_handler(typhoon, 0,
                             (int (*)(void *))pic_read_irq, isa_pic);

    rtc = rtc_init_sqw(0x70, ali1543_get_irq(ali, 8), tim_irq, 1980);

    i8042_init(ali1543_get_irq(ali, 1), ali1543_get_irq(ali, 12), 0x60);

    if (cirrus_vga_enabled && !nographic) {
        ram_addr_t vga_bios_offset;
        int vga_bios_size, ret;

        pci_cirrus_vga_init(hose0,
                            phys_ram_base + vga_ram_addr,
                            vga_ram_addr, vga_ram_size);
        /* VGA BIOS load */
        snprintf(buf, sizeof(buf), "%s/%s", bios_dir, VGABIOS_CIRRUS_FILENAME);
        vga_bios_size = get_image_size(buf);
        if (vga_bios_size <= 0 || vga_bios_size > 65536)
            goto vga_bios_error;
        vga_bios_offset = qemu_ram_alloc(65536);

        ret = load_image(buf, phys_ram_base + vga_bios_offset);
        if (ret != vga_bios_size) {
vga_bios_error:
            fprintf(stderr, "qemu: could not load VGA BIOS '%s'\n", buf);
            exit(1);
        }

        /* setup basic memory access */
        cpu_register_physical_memory(isa_mem_base + 0xc0000, 0x10000,
                                     vga_bios_offset | IO_MEM_ROM);

        rtc_set_memory(rtc, 0x17, 1);
    }

    es40_cpu_reset(env);
}


QEMUMachine es40_rombuild_machine = {
    .name = "es40-rombuild",
    .desc = "Alpha es40 rom builder",
    .init = es40_rombuild_init,
    .ram_require = 16 << 20
};

QEMUMachine es40_machine = {
    .name = "es40",
    .desc = "Alpha es40",
    .init = es40_init,
    .ram_require = VGA_RAM_SIZE + (64 << 20)
};
