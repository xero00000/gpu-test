// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#define __KERNEL__
#include "unlock_610.c"

static u32 regs[0x900000/4];
static const char *asset_root;
static struct unlock_transaction_work *last_work;
static bool full_active;


static struct {
    u16 cmd,saved;
    unsigned long now;
    int allocs,live,frees,fail_alloc,fail_mask,flrs,fail_reset,starts,boot_mode,bar_bad,barriers,writes;
    int gsp_starts,fwsec_mode,master_fail,selector_writes,ss1_writes,fw_live;
    int pending,lost_identity,corrupt_selector,alien_wpr;
    int fw_requests,fail_fw,fail_claim,fail_map,fail_save,fail_context,pins,selector_bad;
    int restore_mask_fail,late_devres;
    int group_requests,fail_group,group_live,group_before,cache_records,cache_released;
    int switch_stuck,dma_commands,dma_stall_after;
    int reset_delay,reset_reads[2],reset_pending[2],reset_post_fault;
    int reset_locked_until_flr,hold_writes,bar_ready_after,bar_ready_reads,wpr_after;
    int post_gfw,post_gfw_reads,gsp_post_reset_writes;
    int fwsec_wpr;u32 fwsec_wpr_lo,fwsec_wpr_hi; /* WPR2 that FWSEC-FRTS enables */
    int apertures,regions;
    char log[32768];
} fake;
void *kzalloc(size_t n,int flags) {
    if(fake.fail_context)return NULL;
    last_work=calloc(1,n);return last_work;
}
void test_device_log(const char *format, ...) {
    va_list args;
    size_t at=strlen(fake.log);
    va_start(args,format);
    vsnprintf(fake.log+at,sizeof(fake.log)-at,format,args);
    va_end(args);
}
static u64 mask;
static struct pci_dev dev;

static void setup(struct unlock_transaction_work *w) {
    memset(&fake,0,sizeof(fake)); memset(regs,0,sizeof(regs)); memset(w,0,sizeof(*w));
    mask=0xffffffffffffull; dev=(struct pci_dev){.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffull},.vendor=0x10de,.device=0x1e81,.refs=1};
    w->pdev=&dev; w->bar=regs; w->bar_len=sizeof(regs); w->mapped=true;
    fake.cmd=2;fake.saved=2;regs[(SEC2_BASE+0xf4)/4]=1u<<31;regs[(0x110000+0xf4)/4]=1u<<31;
    regs[(SEC2_BASE+0x118)/4]=2;regs[(0x110000+0x118)/4]=2;
    regs[GU_FEAT_SS0_OFFSET/4]=0x88888888;regs[GU_FEAT_SS1_OFFSET/4]=0;
    regs[0x1fa824/4]=0xfffffff0;regs[0x1fa828/4]=0;
    regs[0]=0x172000a1;
}
void *dma_alloc_coherent(struct device *d,size_t n,dma_addr_t *a,int flags) {
    fake.allocs++; if(fake.fail_alloc==fake.allocs)return NULL;
    void *p=calloc(1,n); assert(p);
    *a=(d->coherent_dma_mask<=0xffffffffull?0x100000ull:0x100000000ull)+fake.allocs*0x10000ull;
    assert(*a+n-1<=d->coherent_dma_mask);fake.live++;return p;
}
void dma_free_coherent(struct device *d,size_t n,void *p,dma_addr_t a) {
    assert(p && a>=0x100000ull);free(p);fake.live--;fake.frees++;
}
int dma_set_mask(struct device *d,u64 v) {
    *d->dma_mask=v;return fake.fail_mask==1 || (fake.restore_mask_fail && v>0xffffffffull)?-EIO:0;
}
int dma_set_coherent_mask(struct device *d,u64 v) { d->coherent_dma_mask=v;return fake.fail_mask==2?-EIO:0; }
void dma_wmb(void) { fake.barriers++; }
void usleep_range(unsigned long a,unsigned long b) {fake.now+=a;}
ktime_t ktime_get(void) {return fake.now;}
u32 readl(const void *p) {
    size_t n=((const u8 *)p-(const u8 *)regs)/4;assert(n<ARRAY_SIZE(regs));
    if(fake.flrs && !n && fake.bar_ready_after) {
        fake.bar_ready_reads++;
        if(fake.bar_ready_after<0 || fake.bar_ready_reads<fake.bar_ready_after)
            return fake.bar_ready_after==-2?0x12345678:0xbadf0000;
    }
    if(fake.flrs && n==GU_GFW_CPUCTL_OFFSET/4 && fake.post_gfw==1 && ++fake.post_gfw_reads==5) {
        regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;regs[GU_GFW_BOOT_OFFSET/4]=0xff;
    }
    int eng=n==(SEC2_BASE+0x3c0)/4?0:n==0x1103c0/4?1:-1;
    if(eng>=0 && fake.reset_pending[eng] && ++fake.reset_reads[eng]>=fake.reset_delay) {
        regs[n]=1;fake.reset_pending[eng]=0;
    }
    return regs[n];
}

static u32 imem[2][0x10000/4],dmem[2][0x10000/4],imem_at[2],dmem_at[2];
static int scenario;
void writel(u32 v,void *p) {
 size_t off=(u8 *)p-(u8 *)regs;
 assert(off<sizeof(regs));
 assert(off!=0x823804 && off!=0x82381c && off!=0x823820);
 regs[off/4]=v;fake.writes++;
 for(int eng=0;eng<2;eng++) {
  u32 base=eng?SEC2_BASE:0x110000;
  if(off==base+0x180)imem_at[eng]=(v&0xffff)/4;
  if(off==base+0x184){assert(imem_at[eng]<ARRAY_SIZE(imem[eng]));imem[eng][imem_at[eng]++]=v;}
  if(off==base+0x1c0)dmem_at[eng]=(v&0xffff)/4;
  if(off==base+0x1c4){assert(dmem_at[eng]<ARRAY_SIZE(dmem[eng]));dmem[eng][dmem_at[eng]++]=v;}
  if(off==base+0x3c0 && !(v&1)) regs[(base+0x100)/4]=0x10;
  if(off==base+0x100 || off==base+0x130) {
   struct unlock_transaction_work *w=last_work;
   if(!eng) {
    fake.gsp_starts++;
    assert(dmem[0][8]==4 && dmem[0][9]==lower_32_bits(w->fwsec_dma));
    assert(dmem[0][10]==upper_32_bits(w->fwsec_dma));
    assert(dmem[0][12]==0x400 && dmem[0][13]==0x400 && dmem[0][14]==0x9600 && dmem[0][18]==0x3f0);
    assert(regs[(0x110600+4*4)/4]==0x15);
    assert(imem[0][0xfd00/4]==0x004000d0);
    regs[(base+0x100)/4]=scenario==1?2:0x10;
    regs[0x1fa824/4]=(u32)(w->layout.frts_offset>>12)<<4;
    regs[0x1fa828/4]=(u32)((w->layout.wpr_end-0x20000)>>12)<<4;
    if(scenario==2)regs[0x1fa828/4]=0;
   } else {
    fake.starts++;
    assert(!memcmp(imem[1],w->booter_cpu,0x8500));
    assert(!memcmp(dmem[1],(u8 *)w->booter_cpu+0x8500,0x6200));
    assert(!memcmp((u8 *)w->booter_cpu+0x8700,w->booter_fw->data+72+0xe700,16));
    assert(regs[(base+0x104)/4]==0);
    assert(regs[(base+0x40)/4]==lower_32_bits(w->meta_dma));
    assert(regs[(base+0x44)/4]==upper_32_bits(w->meta_dma));
    regs[(base+0x100)/4]=scenario==3?2:0x10;
    regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;
    regs[GU_FEAT_SS0_OFFSET/4]=scenario==4?0x100002:0x88888888;
    regs[GU_FEAT_SS1_OFFSET/4]=8;
    regs[0x1fa824/4]=0x1ffffe00;
    regs[0x1fa828/4]=scenario==5?0x1ffee00:0;
   }
  }
 }
}
void *devres_open_group(struct device *d,void *id,int flags) {
    if(++fake.group_requests==fake.fail_group)return NULL;
    assert(!fake.group_live && !id);
    fake.group_live=1;fake.group_before=d->devres_head.count;
    d->devres_head.count++; /* open group marker */
    return &fake.group_live;
}
int devres_release_group(struct device *d,void *id) {
    assert(fake.group_live && id==&fake.group_live);
    int released=d->devres_head.count-fake.group_before-1;
    fake.cache_released+=released;d->devres_head.count=fake.group_before;
    fake.group_live=0;return released;
}
int request_firmware(const struct firmware **f,const char *n,struct device *d) {
    if(++fake.fw_requests==fake.fail_fw)return -ENOENT;
    if(!asset_root)return -ENOENT;
    char path[4096];
    int length=snprintf(path,sizeof(path),"%s/%s",asset_root,strrchr(n,'/')+1);
    assert(length>0 && (size_t)length<sizeof(path));
    FILE *file=fopen(path,"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);assert(!fseek(file,0,SEEK_SET));
    struct firmware *loaded=malloc(sizeof(*loaded));u8 *data=malloc(size);assert(loaded && data);
    assert(fread(data,1,size,file)==(size_t)size);fclose(file);
    *loaded=(struct firmware){size,data};*f=loaded;fake.fw_live++;
    /* Linux CONFIG_FW_CACHE: successful request adds a device resource;
     * release_firmware frees the image, not this suspend-cache record. */
    d->devres_head.count++;fake.cache_records++;return 0;
}
void release_firmware(const struct firmware *f) {if(f){free((void *)f->data);free((void *)f);fake.fw_live--;}}
void __module_get(void *p) {fake.pins++;}
int pci_read_config_word(struct pci_dev *p,int r,u16 *v) {*v=r==PCI_COMMAND?fake.cmd:r==PCI_VENDOR_ID?0x10de:0;return 0;}
int pci_read_config_dword(struct pci_dev *p,int r,u32 *v) {
    *v=r==PCI_VENDOR_ID?(fake.flrs && fake.lost_identity?~0u:0x1e8110de):(u32)fake.bar_bad;return 0;
}
int pci_write_config_word(struct pci_dev *p,int r,u16 v) {assert(r==PCI_COMMAND);fake.cmd=v;return 0;}
int pcie_capability_read_word(struct pci_dev *p,int r,u16 *v) {*v=r==PCI_EXP_DEVSTA?fake.pending:0;return 0;}
int pcie_capability_read_dword(struct pci_dev *p,int r,u32 *v) {*v=PCI_EXP_DEVCAP_FLR;return 0;}
int pci_is_pcie(struct pci_dev *p) {return 1;}
int pci_select_bars(struct pci_dev *p,int r) {return 7;}
int pci_request_region(struct pci_dev *p,int bar,const char *n) {
    assert(!bar); /* only BAR0: the display card's framebuffer keeps BAR1 */
    if(fake.fail_claim)return -EBUSY;
    fake.regions++;return 0;
}
void pci_release_region(struct pci_dev *p,int bar) {assert(!bar && fake.regions==1);fake.regions--;}
int aperture_remove_conflicting_pci_devices(struct pci_dev *p,const char *n) {
    /* The framebuffer stays through FWSEC and Booter; it goes right before FLR. */
    assert(!fake.flrs && !(fake.cmd&4));
    if(full_active)assert(fake.starts==1);
    fake.apertures++;return 0;
}
int pci_enable_device_mem(struct pci_dev *p) {fake.cmd|=2;return 0;}
void pci_disable_device(struct pci_dev *p) {}
resource_size_t pci_resource_len(struct pci_dev *p,int r) {return sizeof(regs);}
void *pci_iomap(struct pci_dev *p,int r,unsigned long n) {return fake.fail_map?NULL:regs;}
void pci_iounmap(struct pci_dev *p,void *v) {}
void *pci_map_rom(struct pci_dev *p,size_t *n) {return NULL;}
void pci_unmap_rom(struct pci_dev *p,void *v) {}
int pci_save_state(struct pci_dev *p) {fake.saved=fake.cmd;return fake.fail_save?-EIO:0;}
void pci_restore_state(struct pci_dev *p) {assert(!(fake.saved&4));fake.cmd=fake.saved;}
void pci_set_master(struct pci_dev *p) {if(!fake.master_fail)fake.cmd|=4;}
void pci_clear_master(struct pci_dev *p) {fake.cmd&=~4;}
int pcie_flr(struct pci_dev *p) {assert(0 && "TU104 trial must not PF-FLR");return -EIO;}
struct pci_dev *pci_dev_get(struct pci_dev *p) {p->refs++;return p;}
void pci_dev_put(struct pci_dev *p) {p->refs--;}

static void prepare(void) {
 struct unlock_transaction_work w;
 setup(&w);last_work=NULL;scenario=0;
 memset(imem,0,sizeof(imem));memset(dmem,0,sizeof(dmem));
 memset(imem_at,0,sizeof(imem_at));memset(dmem_at,0,sizeof(dmem_at));
 regs[0]=0x164000a1;regs[0x100ce0/4]=0x8a;
 regs[0x110108/4]=0xff;regs[0x110610/4]=0x10;regs[0x1100f4/4]=1u<<31;
 regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;regs[GU_FEAT_SS0_OFFSET/4]=0x100002;regs[GU_FEAT_SS1_OFFSET/4]=5;
 regs[GU_GFW_PLM_OFFSET/4]=0xffffff8f;regs[GU_GFW_BOOT_OFFSET/4]=0x3ff;regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
 FILE *f=fopen("board-rom.bin","rb");assert(f);assert(fread((u8 *)regs+0x300000,1,1024*1024,f)==1024*1024);fclose(f);
}
static int attempt(void) {
 struct gpu_unlock_610_shared shared={0};
 int ret=gpu_unlock_610_load_shared(&dev,&shared);
 if(ret)return ret;
 ret=gpu_unlock_610_unlock(&dev,&shared);
 gpu_unlock_610_release_shared(&shared);return ret;
}
static void clean_assert(void) {
 assert(fake.live==0 && fake.fw_live==0 && fake.regions==0 && fake.pins==0);
 assert(fake.cmd==2 && mask==0xffffffffffffull && dev.dev.coherent_dma_mask==0xfffffffffull);
 assert(dev.dev.devres_head.count==0 && dev.refs==1);
}
static void release_retained_for_test(void) {
 /* Test-only teardown after checking production retention. Never called by
  * the real module: these allocations must survive until hardware poweroff. */
 assert(last_work && last_work->retained && fake.pins==1 && fake.live>0 && fake.regions==1);
 assert(!fake.frees);
 struct unlock_transaction_work *w=last_work;w->firmware_started=false;
 w->retained=false;w->quiesced=true;assert(!unlock_transaction_unwind(w));
 pci_dev_put(w->pdev);free(w);fake.pins=0;clean_assert();
}
int main(void) {
 asset_root="assets";
 prepare();assert(!attempt());assert(fake.starts==1 && fake.gsp_starts==1);clean_assert();
 for(int missing=1;missing<=7;missing++) {
  prepare();fake.fail_fw=missing;assert(attempt());assert(!fake.starts && !fake.gsp_starts);clean_assert();
 }
 prepare();((u8 *)regs)[0x300000]^=1;assert(attempt());assert(!fake.gsp_starts && !fake.starts);clean_assert();
 prepare();regs[0]=0x162000a1;assert(attempt());assert(!fake.gsp_starts);clean_assert();
 prepare();regs[0x100ce0/4]=0x9a;assert(attempt());assert(!fake.gsp_starts);clean_assert();
 for(int mode=1;mode<=5;mode++) {
  prepare();scenario=mode;assert(attempt());
  if(mode<=2)assert(fake.starts==0);else assert(fake.starts==1);
  release_retained_for_test();
 }
 for(int failure=1;failure<=6;failure++) {
  prepare();fake.fail_alloc=failure;assert(attempt());assert(!fake.gsp_starts);clean_assert();
 }
 puts("TU104 mock transaction: success, seven missing assets, ROM/chip/VRAM rejection, five post-start failures, allocation failures passed. Hardware execution is not tested.");
}
