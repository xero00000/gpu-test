// SPDX-License-Identifier: MIT
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Bounded, process-owned buffers. Run only after the trial's boot checks.
static void ck(cudaError_t e, const char *s) {
    if (e != cudaSuccess) { std::fprintf(stderr,"%s: %s\n",s,cudaGetErrorString(e)); std::exit(1); }
}
static void dc(CUresult e, const char *s) {
    if (e != CUDA_SUCCESS) { const char *why=nullptr; cuGetErrorString(e,&why); std::fprintf(stderr,"%s: %s\n",s,why?why:"unknown"); std::exit(1); }
}
static const char ptx[] = R"PTX(
.version 7.0
.target sm_80
.address_size 64
.visible .entry peer_transform(.param .u64 source, .param .u64 destination, .param .u32 count) {
    .reg .pred p;
    .reg .b32 i,t,b,d,n,v;
    .reg .b64 src,dst,offset,a,z;
    ld.param.u64 src,[source];
    ld.param.u64 dst,[destination];
    ld.param.u32 n,[count];
    mov.u32 t,%tid.x;
    mov.u32 b,%ctaid.x;
    mov.u32 d,%ntid.x;
    mad.lo.u32 i,b,d,t;
    setp.ge.u32 p,i,n;
    @p bra done;
    mul.wide.u32 offset,i,4;
    add.u64 a,src,offset;
    add.u64 z,dst,offset;
    ld.global.u32 v,[a];
    xor.b32 v,v,0x7f4a7c15;
    st.global.u32 [z],v;
done:
    ret;
}
)PTX";
struct Card { int id; const char *pci; unsigned *src,*dst; std::vector<unsigned> pattern; CUmodule module; CUfunction function; };
int main(int argc, char **argv) {
    if(argc==2 && std::strcmp(argv[1],"--validate-ptx")==0) {
        int id; ck(cudaDeviceGetByPCIBusId(&id,"0000:04:00.0"),"PTX target lookup");
        ck(cudaSetDevice(id),"PTX validation context"); ck(cudaFree(nullptr),"initialize primary context");
        dc(cuInit(0),"initialize driver"); CUmodule module; CUfunction function;
        dc(cuModuleLoadData(&module,ptx),"validate PTX compilation");
        dc(cuModuleGetFunction(&function,module,"peer_transform"),"validate kernel entry");
        dc(cuModuleUnload(module),"unload PTX validation module");
        std::puts("PTX compilation and entry point passed; no peer mapping or kernel launch."); return 0;
    }
    if(argc!=1) return 2;
    constexpr unsigned n=65536, salt=0x7f4a7c15;
    constexpr size_t bytes=n*sizeof(unsigned);
    Card cards[2]{};
    cards[0].pci="0000:04:00.0"; cards[1].pci="0000:2b:00.0";
    // Two-card process: the guarded launcher hides the unqualified TU104.
    int count=0; ck(cudaGetDeviceCount(&count),"visible device count");
    if(count!=2) { std::fprintf(stderr,"Use run_ampere_p2p.py for this probe\n"); return 2; }
    int all[2]; const char *pcis[]={cards[0].pci,cards[1].pci};
    for(int i=0;i<2;++i) ck(cudaDeviceGetByPCIBusId(&all[i],pcis[i]),"PCI lookup");
    for(int i=0;i<2;++i) {
        int can=0; ck(cudaDeviceCanAccessPeer(&can,all[i],all[1-i]),"query peer");
        std::printf("{\"type\":\"capability\",\"accessor\":\"%s\",\"owner\":\"%s\",\"can_access\":%d}\n",pcis[i],pcis[1-i],can);
        std::fflush(stdout);
        if(!can) return 2;
    }
    dc(cuInit(0),"driver initialization");
    for(int i=0;i<2;++i) {
        auto &c=cards[i]; ck(cudaDeviceGetByPCIBusId(&c.id,c.pci),"target lookup");
        ck(cudaSetDevice(c.id),"set device");
        cudaDeviceProp prop{}; ck(cudaGetDeviceProperties(&prop,c.id),"properties");
        if(prop.major!=8 || prop.minor!=6) return 2;
        ck(cudaMalloc(reinterpret_cast<void **>(&c.src),bytes),"source allocation");
        ck(cudaMalloc(reinterpret_cast<void **>(&c.dst),bytes),"destination allocation");
        c.pattern.resize(n); unsigned state=0x38167a9u+unsigned(i)*0x193782fu;
        for(auto &v:c.pattern) { state^=state<<13; state^=state>>17; state^=state<<5; v=state; }
        ck(cudaMemcpy(c.src,c.pattern.data(),bytes,cudaMemcpyHostToDevice),"source initialization");
        ck(cudaMemset(c.dst,0xa7,bytes),"destination poisoning");
        dc(cuModuleLoadData(&c.module,ptx),"load peer kernel");
        dc(cuModuleGetFunction(&c.function,c.module,"peer_transform"),"get peer kernel");
    }
    for(int i=0;i<2;++i) { ck(cudaSetDevice(cards[i].id),"enable device"); ck(cudaDeviceEnablePeerAccess(cards[1-i].id,0),"enable peer mapping"); }
    std::vector<unsigned> readback(n);
    for(int i=0;i<2;++i) for(int write=0;write<2;++write) {
        auto &local=cards[i]; auto &remote=cards[1-i];
        Card &source=write?local:remote, &destination=write?remote:local;
        ck(cudaSetDevice(destination.id),"poison owner"); ck(cudaMemset(destination.dst,0xa7,bytes),"poison destination");
        ck(cudaSetDevice(local.id),"kernel accessor");
        void *args[]={&source.src,&destination.dst,(void*)&n};
        dc(cuLaunchKernel(local.function,(n+255)/256,1,1,256,1,1,0,nullptr,args,nullptr),"launch peer kernel");
        ck(cudaDeviceSynchronize(),"peer kernel completion");
        ck(cudaSetDevice(destination.id),"readback owner");
        ck(cudaMemcpy(readback.data(),destination.dst,bytes,cudaMemcpyDeviceToHost),"destination readback");
        for(unsigned j=0;j<n;++j) if(readback[j]!=(source.pattern[j]^salt)) { std::fprintf(stderr,"Peer kernel destination mismatch at %u\n",j); return 2; }
        ck(cudaSetDevice(source.id),"source owner");
        ck(cudaMemcpy(readback.data(),source.src,bytes,cudaMemcpyDeviceToHost),"source readback");
        if(readback!=source.pattern) { std::fprintf(stderr,"Source changed\n"); return 2; }
        std::printf("{\"type\":\"kernel_check\",\"accessor\":\"%s\",\"operation\":\"remote_%s\",\"owner\":\"%s\",\"bytes\":%zu,\"correct\":true,\"source_unchanged\":true}\n",local.pci,write?"write":"read",remote.pci,bytes);
        std::fflush(stdout);
    }
    for(int i=0;i<2;++i) { ck(cudaSetDevice(cards[i].id),"cleanup peer context"); ck(cudaDeviceDisablePeerAccess(cards[1-i].id),"disable process peer mapping"); }
    for(auto &c:cards) { ck(cudaSetDevice(c.id),"cleanup device"); dc(cuModuleUnload(c.module),"unload process kernel"); ck(cudaFree(c.src),"free source"); ck(cudaFree(c.dst),"free destination"); }
    return 0;
}
