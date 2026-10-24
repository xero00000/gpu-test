// SPDX-License-Identifier: GPL-2.0-only
// GET-only RM controls, with client/device handles using the reviewed helper.
#define main issue_rate_original_main
#include "../rm_issue_rate.c"
#undef main

typedef struct { NvU32 ids[32]; } Attached;
typedef struct {
    NvU32 ids[32], count, caps, optimalReadCEs, optimalWriteCEs;
    NvU8 status[9];
    NvU32 busPeerIds[1024], busEgmPeerIds[1024];
} P2pCaps;
typedef struct {
    NvBool enabled, writeCombined;
    NvU64 start __attribute__((aligned(8)));
    NvU64 size __attribute__((aligned(8)));
} StaticBar1;
_Static_assert(sizeof(Attached)==128,"595 GET attached ABI");
_Static_assert(sizeof(P2pCaps)==8348,"595 GET P2P caps v2 ABI");
_Static_assert(sizeof(StaticBar1)==24,"595 GET static BAR1 ABI");
int main(void) {
    int ctl=open("/dev/nvidiactl",O_RDWR|O_CLOEXEC);
    NvHandle client=0;
    if(ctl<0 || !alloc_client(ctl,&client)) return 1;
    Attached attached;memset(&attached,0xff,sizeof(attached));
    ControlResult r=rm_control(ctl,client,client,0x201,&attached,sizeof(attached));
    printf("{\"type\":\"attached\",\"os_error\":%d,\"rm_status\":%u,\"ids\":[",r.os_error,r.rm_status);
    unsigned count=0;
    while(count<32 && attached.ids[count]!=0xffffffffU) {
        printf("%s%u",count?",":"",attached.ids[count]); ++count;
    }
    printf("]}\n");
    if(r.os_error || r.rm_status || count!=3) return 2;
    for(unsigned i=0;i<count;++i) for(unsigned j=i+1;j<count;++j) {
        P2pCaps p={.count=2};p.ids[0]=attached.ids[i];p.ids[1]=attached.ids[j];
        r=rm_control(ctl,client,client,0x12b,&p,sizeof(p));
        printf("{\"type\":\"p2p_caps\",\"ids\":[%u,%u],\"os_error\":%d,\"rm_status\":%u,\"flags\":%u,\"bar1_supported\":%s,\"status\":[",p.ids[0],p.ids[1],r.os_error,r.rm_status,p.caps,(p.caps & (1U<<13))?"true":"false");
        for(unsigned k=0;k<9;++k) printf("%s%u",k?",":"",p.status[k]);
        printf("]}\n");
    }
    for(int i=0;i<3;++i) {
        char path[32];snprintf(path,sizeof(path),"/dev/nvidia%d",i);
        int fd=open(path,O_RDWR|O_CLOEXEC); NvHandle device=0,subdevice=0;
        if(fd<0 || !register_device_fd(fd,ctl) || !alloc_device(ctl,client,i,&device) || !alloc_subdevice(ctl,client,device,&subdevice)) return 3;
        StaticBar1 p={0};r=rm_control(ctl,client,subdevice,0x20801354,&p,sizeof(p));
        printf("{\"type\":\"static_bar1\",\"minor\":%d,\"os_error\":%d,\"rm_status\":%u,\"enabled\":%s,\"write_combined\":%s,\"start\":%llu,\"size\":%llu}\n",i,r.os_error,r.rm_status,p.enabled?"true":"false",p.writeCombined?"true":"false",(unsigned long long)p.start,(unsigned long long)p.size);
        close(fd);
    }
    close(ctl);return 0;
}
