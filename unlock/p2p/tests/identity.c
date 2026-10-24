// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "p2p_trial.h"
struct pci { NvU32 device, subsystem; };
static unsigned reads;
NvU32 osPciReadDword(void *h, NvU32 off) {
    struct pci *p=h; ++reads; assert(off==0 || off==0x2c);
    return off==0 ? p->device : p->subsystem;
}
int main(void) {
    struct pci p={0x248910deU,0x1389196eU}; OBJGPU g={{0,0},&p};
    assert(gpuP2pTrialTarget(NULL)==0);
    assert(gpuP2pTrialTarget(&g)==1 && reads==2);
    p.device=0x221610deU; p.subsystem=0x161219daU;
    assert(gpuP2pTrialTarget(&g)==1 && reads==4);
    p.device=0x1e8110deU;p.subsystem=0x30813842U;
    assert(gpuP2pTrialTarget(&g)==0);
    p.device=0x248910deU;p.subsystem=0x1389196fU;
    assert(gpuP2pTrialTarget(&g)==0);
    p.device=0x221610deU;p.subsystem=0x161219dbU;
    assert(gpuP2pTrialTarget(&g)==0);
    p.device=0xffffffffU;p.subsystem=0xffffffffU;
    assert(gpuP2pTrialTarget(&g)==0);
    g.hPci=NULL;assert(gpuP2pTrialTarget(&g)==0);
    unsigned before=reads;
    g.idInfo.PCIDeviceID=0x248910deU;g.idInfo.PCISubDeviceID=0x1389196eU;
    assert(gpuP2pTrialTarget(&g)==1 && reads==before);
    g.idInfo.PCIDeviceID=0x221610deU;g.idInfo.PCISubDeviceID=0x161219daU;
    assert(gpuP2pTrialTarget(&g)==1 && reads==before);
    g.idInfo.PCIDeviceID=0x1e8110deU;g.idInfo.PCISubDeviceID=0x30813842U;
    assert(gpuP2pTrialTarget(&g)==0 && reads==before);
    g.idInfo.PCIDeviceID=0x248910deU;g.idInfo.PCISubDeviceID=0x1389196fU;
    assert(gpuP2pTrialTarget(&g)==0 && reads==before);
    g.hPci=&p;g.idInfo.PCISubDeviceID=0;
    assert(gpuP2pTrialTarget(&g)==0 && reads==before+2);
    puts("12 lifecycle/identity checks passed; actual helper, mocked read-only PCI data.");
}
