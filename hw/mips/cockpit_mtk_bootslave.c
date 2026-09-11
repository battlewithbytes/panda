/*
 * Experimental, opt-in MediaTek boot-slave analysis device.
 * Clean-room register facts: Lagos native write trace, corroborated by older
 * platform headers. This is NOT a power/reset controller or a complete SoC.
 * Ordering/one-shot validation below is conservative analysis policy, not a
 * claim about undocumented hardware rejection behavior.
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "target/mips/cockpit_mt.h"

#define TYPE_COCKPIT_MTK_BOOTSLAVE "cockpit-mtk-bootslave"
#define BOOTSLAVE(obj) OBJECT_CHECK(BootSlaveState, (obj), TYPE_COCKPIT_MTK_BOOTSLAVE)
#define REGION_SIZE 0x2000
#define BANK_BASE 0x104
#define BANK_STRIDE 0x0c
#define BANK_COUNT 4

typedef struct BootSlaveState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq unused_irq;
    uint8_t storage[REGION_SIZE];
    bool armed[BANK_COUNT];
    bool entry_written[BANK_COUNT];
    uint32_t entry[BANK_COUNT];
} BootSlaveState;

static uint64_t bootslave_read(void *opaque, hwaddr offset, unsigned size)
{
    BootSlaveState *s = opaque;
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < size && offset + i < REGION_SIZE; i++) {
        value |= (uint64_t)s->storage[offset + i] << (8 * i);
    }
    return value;
}

static void bootslave_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    BootSlaveState *s = opaque;
    unsigned i, core, reg;
    for (i = 0; i < size && offset + i < REGION_SIZE; i++) {
        s->storage[offset + i] = value >> (8 * i);
    }
    /* Partial/unaligned writes cannot be composed into a release transaction. */
    for (core = 0; core < BANK_COUNT; core++) {
        hwaddr base = BANK_BASE + core * BANK_STRIDE;
        if (offset >= base + BANK_STRIDE || offset + size <= base) {
            continue;
        }
        if (size != 4 || (offset & 3)) {
            s->armed[core] = s->entry_written[core] = false;
            continue;
        }
        reg = offset - base;
        switch (reg) {
        case 8:
            s->armed[core] = value == 0x5500;
            s->entry_written[core] = false;
            break;
        case 0:
            s->entry[core] = value;
            s->entry_written[core] = s->armed[core];
            break;
        case 4:
            if (value == 1 && s->armed[core] && s->entry_written[core]) {
                /* The generic engine rejects absent or already released cores. */
                cockpit_mt_release_core(core, s->entry[core]);
            }
            s->armed[core] = s->entry_written[core] = false;
            break;
        }
    }
}

static const MemoryRegionOps bootslave_ops = {
    .read = bootslave_read,
    .write = bootslave_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static void bootslave_reset(DeviceState *dev)
{
    BootSlaveState *s = BOOTSLAVE(dev);
    memset(s->storage, 0, sizeof(s->storage));
    memset(s->armed, 0, sizeof(s->armed));
    memset(s->entry_written, 0, sizeof(s->entry_written));
    memset(s->entry, 0, sizeof(s->entry));
    /* Preserve the existing FirmWire peripheral's AP2MD_DUMMY approximation. */
    s->storage[0x300] = 1;
}

static void bootslave_init(Object *obj)
{
    BootSlaveState *s = BOOTSLAVE(obj);
    memory_region_init_io(&s->iomem, obj, &bootslave_ops, s,
                          TYPE_COCKPIT_MTK_BOOTSLAVE, REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    /* configurable_machine connects IRQ slot 0 on every sysbus device. */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->unused_irq);
}

static void bootslave_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = bootslave_reset;
    dc->desc = "Experimental MediaTek boot-slave analysis subset (no power model)";
}

static const TypeInfo bootslave_type = {
    .name = TYPE_COCKPIT_MTK_BOOTSLAVE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BootSlaveState),
    .instance_init = bootslave_init,
    .class_init = bootslave_class_init,
};

static void bootslave_register_types(void)
{
    type_register_static(&bootslave_type);
}
type_init(bootslave_register_types)
