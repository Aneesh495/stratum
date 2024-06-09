#ifndef STRATUM_KERNEL_PCI_H
#define STRATUM_KERNEL_PCI_H

#include <kernel/types.h>

#define PCI_CONFIG_ADDRESS      0x0CF8
#define PCI_CONFIG_DATA         0x0CFC

/* Standard PCI Configuration Registers */
#define PCI_REG_VENDOR_ID       0x00
#define PCI_REG_DEVICE_ID       0x02
#define PCI_REG_COMMAND         0x04
#define PCI_REG_STATUS          0x06
#define PCI_REG_REVISION_ID     0x08
#define PCI_REG_PROG_IF         0x09
#define PCI_REG_SUBCLASS        0x0A
#define PCI_REG_CLASS           0x0B
#define PCI_REG_CACHE_LINE_SIZE 0x0C
#define PCI_REG_LATENCY_TIMER   0x0D
#define PCI_REG_HEADER_TYPE     0x0E
#define PCI_REG_BIST            0x0F
#define PCI_REG_BAR0            0x10
#define PCI_REG_BAR1            0x14
#define PCI_REG_BAR2            0x18
#define PCI_REG_BAR3            0x1C
#define PCI_REG_BAR4            0x20
#define PCI_REG_BAR5            0x24
#define PCI_REG_CAPABILITIES_PTR 0x34
#define PCI_REG_INTERRUPT_LINE  0x3C
#define PCI_REG_INTERRUPT_PIN   0x3D

/* PCI Command Register Bits */
#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEMORY_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)
#define PCI_CMD_INT_DISABLE     (1 << 10)

/* PCI Status Register Bits */
#define PCI_STATUS_CAPABILITIES (1 << 4)

/* PCI Capability IDs */
#define PCI_CAP_ID_PM           0x01
#define PCI_CAP_ID_AGP          0x02
#define PCI_CAP_ID_VPD          0x03
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_VNDR         0x09
#define PCI_CAP_ID_SHPC         0x0C
#define PCI_CAP_ID_SSVID        0x0D
#define PCI_CAP_ID_AGP3         0x0E
#define PCI_CAP_ID_SECDEV       0x0F
#define PCI_CAP_ID_EXP          0x10
#define PCI_CAP_ID_MSIX         0x11
#define PCI_CAP_ID_SATA         0x12
#define PCI_CAP_ID_AF           0x13

#define PCI_MAX_DEVICES         64

typedef struct {
    uint64_t base_phys;
    uint64_t size;
    void    *base_virt;
    bool     is_io;
    bool     is_64bit;
    bool     is_prefetchable;
} pci_bar_t;

typedef struct pci_cap {
    uint8_t         id;
    uint8_t         offset;
    uint8_t         length;
    struct pci_cap *next;
} pci_cap_t;

typedef struct pci_device {
    uint8_t            bus;
    uint8_t            slot;
    uint8_t            func;
    uint16_t           vendor_id;
    uint16_t           device_id;
    uint8_t            class_code;
    uint8_t            subclass;
    uint8_t            prog_if;
    uint8_t            revision_id;
    uint8_t            header_type;
    uint8_t            irq_line;
    uint8_t            irq_pin;
    pci_bar_t          bars[6];
    uint8_t            num_bars;
    pci_cap_t         *caps;
    struct pci_device *next;
} pci_device_t;

void          pci_init(void);
uint8_t       pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t      pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t      pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void          pci_write_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val);
void          pci_write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void          pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
void          pci_enable_bus_mastering(pci_device_t *dev);
bool          pci_configure_msix(pci_device_t *dev, uint16_t entry, uint8_t vector, uint8_t apic_id);
void          pci_disable_msix(pci_device_t *dev);
uint8_t       pci_find_capability(pci_device_t *dev, uint8_t cap_id);
pci_device_t *pci_get_device_list(void);

#endif /* STRATUM_KERNEL_PCI_H */
