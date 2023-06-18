#include "hw/hw.h"
#include "hw/boards.h"
#include "qemu-timer.h"

#include "exec-all.h"

void register_machines(void)
{
    qemu_register_machine(&es40_machine);
    qemu_register_machine(&es40_rombuild_machine);
}

void cpu_save(QEMUFile *f, void *opaque)
{
    /* FIXME: todo  */
    abort();
}

int cpu_load(QEMUFile *f, void *opaque, int version_id)
{
    return -EINVAL;
}
