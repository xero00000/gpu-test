// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include "unlock_610.h"
static char *targets;
module_param(targets, charp, 0400);
MODULE_PARM_DESC(targets, "Only 0000:05:00.0 EVGA TU104 10de:1e81 3842:3081 is accepted");
static int __init tu104_init(void)
{
 struct pci_dev *p; struct gpu_unlock_610_shared shared = {0}; int ret;
 if (!targets || strcmp(targets,"0000:05:00.0")) return -EINVAL;
 p=pci_get_domain_bus_and_slot(0,5,PCI_DEVFN(0,0));
 if (!p) return -ENODEV;
 pci_dev_lock(p);
 if (p->vendor!=0x10de || p->device!=0x1e81 || p->subsystem_vendor!=0x3842 || p->subsystem_device!=0x3081 || p->driver || p->dev.driver) {ret=-ENODEV;goto out;}
 ret=gpu_unlock_610_load_shared(p,&shared);
 if (!ret) {ret=gpu_unlock_610_unlock(p,&shared);gpu_unlock_610_release_shared(&shared);}
 if (ret) pci_err(p,"gpu_unlock_tu104: unlock failed (%d)\n",ret);
 else pci_info(p,"gpu_unlock_tu104: unlocked and quiesced\n");
 out: pci_dev_unlock(p);pci_dev_put(p);
 /* A retained failure pins this module, its region and DMA memory. Let the
  * other GPUs boot; do not turn a failed trial into a module-unload UAF. */
 return 0;
}
static void __exit tu104_exit(void) {}
module_init(tu104_init);module_exit(tu104_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Board-specific TU104 pre-driver compute unlock experimental trial");
