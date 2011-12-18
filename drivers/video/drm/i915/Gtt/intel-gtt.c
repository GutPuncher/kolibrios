/*
 * Intel GTT (Graphics Translation Table) routines
 *
 * Caveat: This driver implements the linux agp interface, but this is far from
 * a agp driver! GTT support ended up here for purely historical reasons: The
 * old userspace intel graphics drivers needed an interface to map memory into
 * the GTT. And the drm provides a default interface for graphic devices sitting
 * on an agp port. So it made sense to fake the GTT support as an agp port to
 * avoid having to create a new api.
 *
 * With gem this does not make much sense anymore, just needlessly complicates
 * the code. But as long as the old graphics stack is still support, it's stuck
 * here.
 *
 * /fairy-tale-mode off
 */

#include <linux/module.h>
#include <errno-base.h>
#include <linux/pci.h>
#include <linux/kernel.h>
//#include <linux/pagemap.h>
//#include <linux/agp_backend.h>
//#include <asm/smp.h>
#include <linux/spinlock.h>
#include "agp.h"
#include "intel-agp.h"
#include "intel-gtt.h"

#include <syscall.h>

struct pci_dev *
pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);

static bool intel_enable_gtt(void);


#define PG_SW       0x003
#define PG_NOCACHE  0x018

#define PCI_VENDOR_ID_INTEL             0x8086
#define PCI_DEVICE_ID_INTEL_82830_HB    0x3575
#define PCI_DEVICE_ID_INTEL_82845G_HB   0x2560


#define AGP_NORMAL_MEMORY 0

#define AGP_USER_TYPES (1 << 16)
#define AGP_USER_MEMORY (AGP_USER_TYPES)
#define AGP_USER_CACHED_MEMORY (AGP_USER_TYPES + 1)

static inline uint8_t __raw_readb(const volatile void __iomem *addr)
{
    return *(const volatile uint8_t __force *) addr;
}

static inline uint16_t __raw_readw(const volatile void __iomem *addr)
{
    return *(const volatile uint16_t __force *) addr;
}

static inline uint32_t __raw_readl(const volatile void __iomem *addr)
{
    return *(const volatile uint32_t __force *) addr;
}

#define readb __raw_readb
#define readw __raw_readw
#define readl __raw_readl


static inline void __raw_writeb(uint8_t b, volatile void __iomem *addr)
{    *(volatile uint8_t __force *) addr = b;}

static inline void __raw_writew(uint16_t b, volatile void __iomem *addr)
{    *(volatile uint16_t __force *) addr = b;}

static inline void __raw_writel(uint32_t b, volatile void __iomem *addr)
{    *(volatile uint32_t __force *) addr = b;}

static inline void __raw_writeq(__u64 b, volatile void __iomem *addr)
{    *(volatile __u64 *)addr = b;}

#define writeb __raw_writeb
#define writew __raw_writew
#define writel __raw_writel
#define writeq __raw_writeq

static inline int pci_read_config_word(struct pci_dev *dev, int where,
                    u16 *val)
{
    *val = PciRead16(dev->busnr, dev->devfn, where);
    return 1;
}

static inline int pci_read_config_dword(struct pci_dev *dev, int where,
                    u32 *val)
{
    *val = PciRead32(dev->busnr, dev->devfn, where);
    return 1;
}

static inline int pci_write_config_word(struct pci_dev *dev, int where,
                    u16 val)
{
    PciWrite16(dev->busnr, dev->devfn, where, val);
    return 1;
}

/*
 * If we have Intel graphics, we're not going to have anything other than
 * an Intel IOMMU. So make the correct use of the PCI DMA API contingent
 * on the Intel IOMMU support (CONFIG_DMAR).
 * Only newer chipsets need to bother with this, of course.
 */
#ifdef CONFIG_DMAR
#define USE_PCI_DMA_API 1
#else
#define USE_PCI_DMA_API 0
#endif

struct intel_gtt_driver {
    unsigned int gen : 8;
    unsigned int is_g33 : 1;
    unsigned int is_pineview : 1;
    unsigned int is_ironlake : 1;
    unsigned int has_pgtbl_enable : 1;
    unsigned int dma_mask_size : 8;
    /* Chipset specific GTT setup */
    int (*setup)(void);
    /* This should undo anything done in ->setup() save the unmapping
     * of the mmio register file, that's done in the generic code. */
    void (*cleanup)(void);
    void (*write_entry)(dma_addr_t addr, unsigned int entry, unsigned int flags);
    /* Flags is a more or less chipset specific opaque value.
     * For chipsets that need to support old ums (non-gem) code, this
     * needs to be identical to the various supported agp memory types! */
    bool (*check_flags)(unsigned int flags);
    void (*chipset_flush)(void);
};

static struct _intel_private {
    struct intel_gtt base;
    const struct intel_gtt_driver *driver;
    struct pci_dev *pcidev; /* device one */
    struct pci_dev *bridge_dev;
    u8 __iomem *registers;
    phys_addr_t gtt_bus_addr;
    phys_addr_t gma_bus_addr;
    u32 PGETBL_save;
    u32 __iomem *gtt;       /* I915G */
    bool clear_fake_agp; /* on first access via agp, fill with scratch */
    int num_dcache_entries;
    void __iomem *i9xx_flush_page;
    char *i81x_gtt_table;
    struct resource ifp_resource;
    int resource_valid;
    struct page *scratch_page;
    dma_addr_t scratch_page_dma;
} intel_private;

#define INTEL_GTT_GEN   intel_private.driver->gen
#define IS_G33          intel_private.driver->is_g33
#define IS_PINEVIEW     intel_private.driver->is_pineview
#define IS_IRONLAKE     intel_private.driver->is_ironlake
#define HAS_PGTBL_EN    intel_private.driver->has_pgtbl_enable

static int intel_gtt_setup_scratch_page(void)
{
    addr_t page;

    page = AllocPage();
    if (page == 0)
        return -ENOMEM;

    intel_private.scratch_page_dma = page;
    intel_private.scratch_page = NULL;

    return 0;
}

static unsigned int intel_gtt_stolen_size(void)
{
    u16 gmch_ctrl;
    u8 rdct;
    int local = 0;
    static const int ddt[4] = { 0, 16, 32, 64 };
    unsigned int stolen_size = 0;

    if (INTEL_GTT_GEN == 1)
        return 0; /* no stolen mem on i81x */

    pci_read_config_word(intel_private.bridge_dev,
                 I830_GMCH_CTRL, &gmch_ctrl);

    if (intel_private.bridge_dev->device == PCI_DEVICE_ID_INTEL_82830_HB ||
        intel_private.bridge_dev->device == PCI_DEVICE_ID_INTEL_82845G_HB) {
        switch (gmch_ctrl & I830_GMCH_GMS_MASK) {
        case I830_GMCH_GMS_STOLEN_512:
            stolen_size = KB(512);
            break;
        case I830_GMCH_GMS_STOLEN_1024:
            stolen_size = MB(1);
            break;
        case I830_GMCH_GMS_STOLEN_8192:
            stolen_size = MB(8);
            break;
        case I830_GMCH_GMS_LOCAL:
            rdct = readb(intel_private.registers+I830_RDRAM_CHANNEL_TYPE);
            stolen_size = (I830_RDRAM_ND(rdct) + 1) *
                    MB(ddt[I830_RDRAM_DDT(rdct)]);
            local = 1;
            break;
        default:
            stolen_size = 0;
            break;
        }
    } else if (INTEL_GTT_GEN == 6) {
        /*
         * SandyBridge has new memory control reg at 0x50.w
         */
        u16 snb_gmch_ctl;
        pci_read_config_word(intel_private.pcidev, SNB_GMCH_CTRL, &snb_gmch_ctl);
        switch (snb_gmch_ctl & SNB_GMCH_GMS_STOLEN_MASK) {
        case SNB_GMCH_GMS_STOLEN_32M:
            stolen_size = MB(32);
            break;
        case SNB_GMCH_GMS_STOLEN_64M:
            stolen_size = MB(64);
            break;
        case SNB_GMCH_GMS_STOLEN_96M:
            stolen_size = MB(96);
            break;
        case SNB_GMCH_GMS_STOLEN_128M:
            stolen_size = MB(128);
            break;
        case SNB_GMCH_GMS_STOLEN_160M:
            stolen_size = MB(160);
            break;
        case SNB_GMCH_GMS_STOLEN_192M:
            stolen_size = MB(192);
            break;
        case SNB_GMCH_GMS_STOLEN_224M:
            stolen_size = MB(224);
            break;
        case SNB_GMCH_GMS_STOLEN_256M:
            stolen_size = MB(256);
            break;
        case SNB_GMCH_GMS_STOLEN_288M:
            stolen_size = MB(288);
            break;
        case SNB_GMCH_GMS_STOLEN_320M:
            stolen_size = MB(320);
            break;
        case SNB_GMCH_GMS_STOLEN_352M:
            stolen_size = MB(352);
            break;
        case SNB_GMCH_GMS_STOLEN_384M:
            stolen_size = MB(384);
            break;
        case SNB_GMCH_GMS_STOLEN_416M:
            stolen_size = MB(416);
            break;
        case SNB_GMCH_GMS_STOLEN_448M:
            stolen_size = MB(448);
            break;
        case SNB_GMCH_GMS_STOLEN_480M:
            stolen_size = MB(480);
            break;
        case SNB_GMCH_GMS_STOLEN_512M:
            stolen_size = MB(512);
            break;
        }
    } else {
        switch (gmch_ctrl & I855_GMCH_GMS_MASK) {
        case I855_GMCH_GMS_STOLEN_1M:
            stolen_size = MB(1);
            break;
        case I855_GMCH_GMS_STOLEN_4M:
            stolen_size = MB(4);
            break;
        case I855_GMCH_GMS_STOLEN_8M:
            stolen_size = MB(8);
            break;
        case I855_GMCH_GMS_STOLEN_16M:
            stolen_size = MB(16);
            break;
        case I855_GMCH_GMS_STOLEN_32M:
            stolen_size = MB(32);
            break;
        case I915_GMCH_GMS_STOLEN_48M:
            stolen_size = MB(48);
            break;
        case I915_GMCH_GMS_STOLEN_64M:
            stolen_size = MB(64);
            break;
        case G33_GMCH_GMS_STOLEN_128M:
            stolen_size = MB(128);
            break;
        case G33_GMCH_GMS_STOLEN_256M:
            stolen_size = MB(256);
            break;
        case INTEL_GMCH_GMS_STOLEN_96M:
            stolen_size = MB(96);
            break;
        case INTEL_GMCH_GMS_STOLEN_160M:
            stolen_size = MB(160);
            break;
        case INTEL_GMCH_GMS_STOLEN_224M:
            stolen_size = MB(224);
            break;
        case INTEL_GMCH_GMS_STOLEN_352M:
            stolen_size = MB(352);
            break;
        default:
            stolen_size = 0;
            break;
        }
    }

    if (stolen_size > 0) {
        dbgprintf("detected %dK %s memory\n",
               stolen_size / KB(1), local ? "local" : "stolen");
    } else {
        dbgprintf("no pre-allocated video memory detected\n");
        stolen_size = 0;
    }

    return stolen_size;
}

static void i965_adjust_pgetbl_size(unsigned int size_flag)
{
    u32 pgetbl_ctl, pgetbl_ctl2;

    /* ensure that ppgtt is disabled */
    pgetbl_ctl2 = readl(intel_private.registers+I965_PGETBL_CTL2);
    pgetbl_ctl2 &= ~I810_PGETBL_ENABLED;
    writel(pgetbl_ctl2, intel_private.registers+I965_PGETBL_CTL2);

    /* write the new ggtt size */
    pgetbl_ctl = readl(intel_private.registers+I810_PGETBL_CTL);
    pgetbl_ctl &= ~I965_PGETBL_SIZE_MASK;
    pgetbl_ctl |= size_flag;
    writel(pgetbl_ctl, intel_private.registers+I810_PGETBL_CTL);
}

static unsigned int i965_gtt_total_entries(void)
{
    int size;
    u32 pgetbl_ctl;
    u16 gmch_ctl;

    pci_read_config_word(intel_private.bridge_dev,
                 I830_GMCH_CTRL, &gmch_ctl);

    if (INTEL_GTT_GEN == 5) {
        switch (gmch_ctl & G4x_GMCH_SIZE_MASK) {
        case G4x_GMCH_SIZE_1M:
        case G4x_GMCH_SIZE_VT_1M:
            i965_adjust_pgetbl_size(I965_PGETBL_SIZE_1MB);
            break;
        case G4x_GMCH_SIZE_VT_1_5M:
            i965_adjust_pgetbl_size(I965_PGETBL_SIZE_1_5MB);
            break;
        case G4x_GMCH_SIZE_2M:
        case G4x_GMCH_SIZE_VT_2M:
            i965_adjust_pgetbl_size(I965_PGETBL_SIZE_2MB);
            break;
        }
    }

    pgetbl_ctl = readl(intel_private.registers+I810_PGETBL_CTL);

    switch (pgetbl_ctl & I965_PGETBL_SIZE_MASK) {
    case I965_PGETBL_SIZE_128KB:
        size = KB(128);
        break;
    case I965_PGETBL_SIZE_256KB:
        size = KB(256);
        break;
    case I965_PGETBL_SIZE_512KB:
        size = KB(512);
        break;
    /* GTT pagetable sizes bigger than 512KB are not possible on G33! */
    case I965_PGETBL_SIZE_1MB:
        size = KB(1024);
        break;
    case I965_PGETBL_SIZE_2MB:
        size = KB(2048);
        break;
    case I965_PGETBL_SIZE_1_5MB:
        size = KB(1024 + 512);
        break;
    default:
        dbgprintf("unknown page table size, assuming 512KB\n");
        size = KB(512);
    }

    return size/4;
}

static unsigned int intel_gtt_total_entries(void)
{
    int size;

    if (IS_G33 || INTEL_GTT_GEN == 4 || INTEL_GTT_GEN == 5)
        return i965_gtt_total_entries();
    else if (INTEL_GTT_GEN == 6) {
        u16 snb_gmch_ctl;

        pci_read_config_word(intel_private.pcidev, SNB_GMCH_CTRL, &snb_gmch_ctl);
        switch (snb_gmch_ctl & SNB_GTT_SIZE_MASK) {
        default:
        case SNB_GTT_SIZE_0M:
            printk(KERN_ERR "Bad GTT size mask: 0x%04x.\n", snb_gmch_ctl);
            size = MB(0);
            break;
        case SNB_GTT_SIZE_1M:
            size = MB(1);
            break;
        case SNB_GTT_SIZE_2M:
            size = MB(2);
            break;
        }
        return size/4;
    } else {
        /* On previous hardware, the GTT size was just what was
         * required to map the aperture.
         */
        return intel_private.base.gtt_mappable_entries;
    }
}

static unsigned int intel_gtt_mappable_entries(void)
{
    unsigned int aperture_size;

    if (INTEL_GTT_GEN == 1) {
        u32 smram_miscc;

        pci_read_config_dword(intel_private.bridge_dev,
                      I810_SMRAM_MISCC, &smram_miscc);

        if ((smram_miscc & I810_GFX_MEM_WIN_SIZE)
                == I810_GFX_MEM_WIN_32M)
            aperture_size = MB(32);
        else
            aperture_size = MB(64);
    } else if (INTEL_GTT_GEN == 2) {
        u16 gmch_ctrl;

        pci_read_config_word(intel_private.bridge_dev,
                     I830_GMCH_CTRL, &gmch_ctrl);

        if ((gmch_ctrl & I830_GMCH_MEM_MASK) == I830_GMCH_MEM_64M)
            aperture_size = MB(64);
        else
            aperture_size = MB(128);
    } else {
        /* 9xx supports large sizes, just look at the length */
        aperture_size = pci_resource_len(intel_private.pcidev, 2);
    }

    return aperture_size >> PAGE_SHIFT;
}

static void intel_gtt_teardown_scratch_page(void)
{
   // FreePage(intel_private.scratch_page_dma);
}

static void intel_gtt_cleanup(void)
{
    intel_private.driver->cleanup();

    FreeKernelSpace(intel_private.gtt);
    FreeKernelSpace(intel_private.registers);

  //  intel_gtt_teardown_scratch_page();
}

static int intel_gtt_init(void)
{
    u32 gtt_map_size;
    int ret;

    ENTER();

    ret = intel_private.driver->setup();
    if (ret != 0)
    {
        LEAVE();
        return ret;
    };


    intel_private.base.gtt_mappable_entries = intel_gtt_mappable_entries();
    intel_private.base.gtt_total_entries = intel_gtt_total_entries();

    /* save the PGETBL reg for resume */
    intel_private.PGETBL_save =
        readl(intel_private.registers+I810_PGETBL_CTL)
            & ~I810_PGETBL_ENABLED;
    /* we only ever restore the register when enabling the PGTBL... */
    if (HAS_PGTBL_EN)
        intel_private.PGETBL_save |= I810_PGETBL_ENABLED;

    dbgprintf("detected gtt size: %dK total, %dK mappable\n",
            intel_private.base.gtt_total_entries * 4,
            intel_private.base.gtt_mappable_entries * 4);

    gtt_map_size = intel_private.base.gtt_total_entries * 4;

    intel_private.gtt = (u32*)MapIoMem(intel_private.gtt_bus_addr,
                    gtt_map_size, PG_SW+PG_NOCACHE);
    if (!intel_private.gtt) {
        intel_private.driver->cleanup();
        FreeKernelSpace(intel_private.registers);
        return -ENOMEM;
    }

    asm volatile("wbinvd");

    intel_private.base.stolen_size = intel_gtt_stolen_size();

    intel_private.base.needs_dmar = USE_PCI_DMA_API && INTEL_GTT_GEN > 2;

    ret = intel_gtt_setup_scratch_page();
    if (ret != 0) {
        intel_gtt_cleanup();
        return ret;
    }

    intel_enable_gtt();

    LEAVE();

    return 0;
}

static bool intel_enable_gtt(void)
{
    u32 gma_addr;
    u8 __iomem *reg;

    if (INTEL_GTT_GEN <= 2)
        pci_read_config_dword(intel_private.pcidev, I810_GMADDR,
                      &gma_addr);
    else
        pci_read_config_dword(intel_private.pcidev, I915_GMADDR,
                      &gma_addr);

    intel_private.gma_bus_addr = (gma_addr & PCI_BASE_ADDRESS_MEM_MASK);

    if (INTEL_GTT_GEN >= 6)
        return true;

    if (INTEL_GTT_GEN == 2) {
        u16 gmch_ctrl;

        pci_read_config_word(intel_private.bridge_dev,
                     I830_GMCH_CTRL, &gmch_ctrl);
        gmch_ctrl |= I830_GMCH_ENABLED;
        pci_write_config_word(intel_private.bridge_dev,
                      I830_GMCH_CTRL, gmch_ctrl);

        pci_read_config_word(intel_private.bridge_dev,
                     I830_GMCH_CTRL, &gmch_ctrl);
        if ((gmch_ctrl & I830_GMCH_ENABLED) == 0) {
            dbgprintf("failed to enable the GTT: GMCH_CTRL=%x\n",
                gmch_ctrl);
            return false;
        }
    }

    /* On the resume path we may be adjusting the PGTBL value, so
     * be paranoid and flush all chipset write buffers...
     */
    if (INTEL_GTT_GEN >= 3)
        writel(0, intel_private.registers+GFX_FLSH_CNTL);

    reg = intel_private.registers+I810_PGETBL_CTL;
    writel(intel_private.PGETBL_save, reg);
    if (HAS_PGTBL_EN && (readl(reg) & I810_PGETBL_ENABLED) == 0) {
        dbgprintf("failed to enable the GTT: PGETBL=%x [expected %x]\n",
            readl(reg), intel_private.PGETBL_save);
        return false;
    }

    if (INTEL_GTT_GEN >= 3)
        writel(0, intel_private.registers+GFX_FLSH_CNTL);

    return true;
}



static void intel_i9xx_setup_flush(void)
{
    /* return if already configured */
    if (intel_private.ifp_resource.start)
        return;

    if (INTEL_GTT_GEN == 6)
        return;

#if 0
    /* setup a resource for this object */
    intel_private.ifp_resource.name = "Intel Flush Page";
    intel_private.ifp_resource.flags = IORESOURCE_MEM;

    /* Setup chipset flush for 915 */
    if (IS_G33 || INTEL_GTT_GEN >= 4) {
        intel_i965_g33_setup_chipset_flush();
    } else {
        intel_i915_setup_chipset_flush();
    }

    if (intel_private.ifp_resource.start)
        intel_private.i9xx_flush_page = ioremap_nocache(intel_private.ifp_resource.start, PAGE_SIZE);
    if (!intel_private.i9xx_flush_page)
        dev_err(&intel_private.pcidev->dev,
            "can't ioremap flush page - no chipset flushing\n");
#endif

}

static void i9xx_chipset_flush(void)
{
    if (intel_private.i9xx_flush_page)
        writel(1, intel_private.i9xx_flush_page);
}

static bool gen6_check_flags(unsigned int flags)
{
    return true;
}

static void gen6_write_entry(dma_addr_t addr, unsigned int entry,
                 unsigned int flags)
{
    unsigned int type_mask = flags & ~AGP_USER_CACHED_MEMORY_GFDT;
    unsigned int gfdt = flags & AGP_USER_CACHED_MEMORY_GFDT;
    u32 pte_flags;

    if (type_mask == AGP_USER_MEMORY)
        pte_flags = GEN6_PTE_UNCACHED | I810_PTE_VALID;
    else if (type_mask == AGP_USER_CACHED_MEMORY_LLC_MLC) {
        pte_flags = GEN6_PTE_LLC_MLC | I810_PTE_VALID;
        if (gfdt)
            pte_flags |= GEN6_PTE_GFDT;
    } else { /* set 'normal'/'cached' to LLC by default */
        pte_flags = GEN6_PTE_LLC | I810_PTE_VALID;
        if (gfdt)
            pte_flags |= GEN6_PTE_GFDT;
    }

    /* gen6 has bit11-4 for physical addr bit39-32 */
    addr |= (addr >> 28) & 0xff0;
    writel(addr | pte_flags, intel_private.gtt + entry);
}

static void gen6_cleanup(void)
{
}

static int i9xx_setup(void)
{
    u32 reg_addr;

    pci_read_config_dword(intel_private.pcidev, I915_MMADDR, &reg_addr);

    reg_addr &= 0xfff80000;

    intel_private.registers = (u8*)MapIoMem(reg_addr, 128 * 4096, PG_SW+PG_NOCACHE);

    if (!intel_private.registers)
        return -ENOMEM;

    if (INTEL_GTT_GEN == 3) {
        u32 gtt_addr;

        pci_read_config_dword(intel_private.pcidev,
                      I915_PTEADDR, &gtt_addr);
        intel_private.gtt_bus_addr = gtt_addr;
    } else {
        u32 gtt_offset;

        switch (INTEL_GTT_GEN) {
        case 5:
        case 6:
            gtt_offset = MB(2);
            break;
        case 4:
        default:
            gtt_offset =  KB(512);
            break;
        }
        intel_private.gtt_bus_addr = reg_addr + gtt_offset;
    }

    intel_i9xx_setup_flush();

    return 0;
}

static const struct intel_gtt_driver sandybridge_gtt_driver = {
    .gen = 6,
    .setup = i9xx_setup,
    .cleanup = gen6_cleanup,
    .write_entry = gen6_write_entry,
    .dma_mask_size = 40,
    .check_flags = gen6_check_flags,
    .chipset_flush = i9xx_chipset_flush,
};

/* Table to describe Intel GMCH and AGP/PCIE GART drivers.  At least one of
 * driver and gmch_driver must be non-null, and find_gmch will determine
 * which one should be used if a gmch_chip_id is present.
 */
static const struct intel_gtt_driver_description {
    unsigned int gmch_chip_id;
    char *name;
    const struct intel_gtt_driver *gtt_driver;
} intel_gtt_chipsets[] = {
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_GT1_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_GT2_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_GT2_PLUS_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_M_GT1_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_M_GT2_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_M_GT2_PLUS_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { PCI_DEVICE_ID_INTEL_SANDYBRIDGE_S_IG,
        "Sandybridge", &sandybridge_gtt_driver },
    { 0, NULL, NULL }
};

static int find_gmch(u16 device)
{
    struct pci_dev *gmch_device;

    gmch_device = pci_get_device(PCI_VENDOR_ID_INTEL, device, NULL);
    if (gmch_device && PCI_FUNC(gmch_device->devfn) != 0) {
        gmch_device = pci_get_device(PCI_VENDOR_ID_INTEL,
                         device, gmch_device);
    }

    if (!gmch_device)
        return 0;

    intel_private.pcidev = gmch_device;
    return 1;
}

int intel_gmch_probe(struct pci_dev *pdev,
                      struct agp_bridge_data *bridge)
{
    int i, mask;
    intel_private.driver = NULL;

    for (i = 0; intel_gtt_chipsets[i].name != NULL; i++) {
        if (find_gmch(intel_gtt_chipsets[i].gmch_chip_id)) {
            intel_private.driver =
                intel_gtt_chipsets[i].gtt_driver;
            break;
        }
    }

    if (!intel_private.driver)
        return 0;

 //   bridge->driver = &intel_fake_agp_driver;
    bridge->dev_private_data = &intel_private;
    bridge->dev = pdev;

    intel_private.bridge_dev = pdev;

    dbgprintf("Intel %s Chipset\n", intel_gtt_chipsets[i].name);

    mask = intel_private.driver->dma_mask_size;
//    if (pci_set_dma_mask(intel_private.pcidev, DMA_BIT_MASK(mask)))
//        dev_err(&intel_private.pcidev->dev,
//            "set gfx device dma mask %d-bit failed!\n", mask);
//    else
//        pci_set_consistent_dma_mask(intel_private.pcidev,
//                        DMA_BIT_MASK(mask));

    /*if (bridge->driver == &intel_810_driver)
        return 1;*/

    if (intel_gtt_init() != 0)
        return 0;

    return 1;
}

