// SPDX-License-Identifier: MIT
#include <cuda_runtime_api.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Bounded process-local allocations/copies only. No driver settings, resets,
// register writes or compute kernels. Optional process-local Ampere peer mappings.
using Clock = std::chrono::steady_clock;
static void ck(cudaError_t status, const char *operation) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "%s: %s\n", operation, cudaGetErrorString(status));
        std::exit(1); // no retry after a CUDA failure
    }
}
struct Card {
    int id;
    const char *pci;
    void *src = nullptr, *dst = nullptr;
    cudaStream_t stream;
    std::vector<unsigned char> expected;
};
static std::string sysfs(const char *pci, const char *field) {
    std::ifstream f(std::string("/sys/bus/pci/devices/") + pci + "/" + field);
    std::string s;
    std::getline(f, s);
    return s;
}
static void verify(Card &card, void *host, size_t bytes) {
    ck(cudaSetDevice(card.id), "verify set device");
    ck(cudaMemcpy(host, card.dst, bytes, cudaMemcpyDeviceToHost), "verify destination");
    if (std::memcmp(host, card.expected.data(), bytes)) {
        std::fprintf(stderr, "destination mismatch %s\n", card.pci);
        std::exit(2);
    }
}
int main(int argc, char **argv) {
    bool enable_peer = argc == 2 && std::strcmp(argv[1], "--enable-ampere-peer") == 0;
    if (argc > 2 || (argc == 2 && !enable_peer)) return 2;
    constexpr size_t capacity = 16 * 1024 * 1024;
    const char *targets[] = {"0000:04:00.0", "0000:2b:00.0"};
    std::vector<Card> cards;
    void *host = nullptr, *scratch = nullptr;
    ck(cudaHostAlloc(&host, capacity, cudaHostAllocPortable), "pinned host");
    ck(cudaHostAlloc(&scratch, capacity, cudaHostAllocPortable), "pinned scratch");
    int runtime = 0, driver = 0;
    ck(cudaRuntimeGetVersion(&runtime), "runtime version");
    ck(cudaDriverGetVersion(&driver), "driver version");
    std::printf("{\"type\":\"protocol\",\"runtime\":%d,\"driver\":%d,\"samples\":5,\"warmups\":1,\"max_device_allocation_bytes\":%zu,\"peer_mapping_enabled\":%s,\"timing\":\"host wall including stream synchronization\"}\n", runtime, driver, 2*capacity, enable_peer ? "true" : "false");
    for (auto pci : targets) {
        Card card;
        card.pci = pci;
        ck(cudaDeviceGetByPCIBusId(&card.id, pci), "PCI selection");
        ck(cudaSetDevice(card.id), "set device");
        cudaDeviceProp prop{};
        ck(cudaGetDeviceProperties(&prop, card.id), "properties");
        ck(cudaStreamCreateWithFlags(&card.stream, cudaStreamNonBlocking), "stream");
        ck(cudaMalloc(&card.src, capacity), "source allocation");
        ck(cudaMalloc(&card.dst, capacity), "destination allocation");
        card.expected.resize(capacity);
        unsigned state = 0x9182A3B4u + unsigned(cards.size()) * 0x71397u;
        for (auto &byte : card.expected) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            byte = static_cast<unsigned char>(state);
        }
        std::memcpy(host, card.expected.data(), capacity);
        ck(cudaMemcpy(card.src, host, capacity, cudaMemcpyHostToDevice), "initialize source");
        std::printf("{\"type\":\"device\",\"index\":%d,\"pci\":\"%s\",\"name\":\"%s\"}\n", card.id, pci, prop.name);
        cards.push_back(std::move(card));
    }
    if (enable_peer) {
        for (auto pair : {std::pair<int,int>{0,1}, {1,0}}) {
            Card &local = cards[pair.first], &remote = cards[pair.second];
            int can = 0;
            ck(cudaDeviceCanAccessPeer(&can, local.id, remote.id), "peer query before enable");
            if (!can) { std::fprintf(stderr, "Ampere peer support unavailable\n"); return 2; }
            ck(cudaSetDevice(local.id), "peer accessor context");
            ck(cudaDeviceEnablePeerAccess(remote.id, 0), "enable Ampere peer mapping");
        }
    }
    auto report = [&](const char *mode, const char *source, const char *destination,
                      size_t bytes, int reps, std::vector<double> times, Card &active) {
        std::sort(times.begin(), times.end());
        std::printf("{\"type\":\"measurement\",\"mode\":\"%s\",\"source\":\"%s\",\"destination\":\"%s\",\"bytes\":%zu,\"reps_per_sample\":%d,\"median_us\":%.6f,\"min_us\":%.6f,\"max_us\":%.6f,\"GBps\":%.6f,\"correct\":true,\"active_link_speed\":\"%s\",\"active_link_width\":\"%s\"}\n",
                    mode, source, destination, bytes, reps, times[2], times.front(), times.back(),
                    bytes / (times[2] * 1000.0), sysfs(active.pci,"current_link_speed").c_str(), sysfs(active.pci,"current_link_width").c_str());
        std::fflush(stdout);
    };
    for (auto &card : cards) {
        ck(cudaSetDevice(card.id), "single set device");
        for (size_t bytes : {size_t(4096), size_t(65536), capacity}) {
            const int reps = bytes == capacity ? 3 : 8;
            std::memcpy(host, card.expected.data(), bytes);
            for (int direction = 0; direction < 2; ++direction) {
                std::vector<double> samples;
                for (int sample = -1; sample < 5; ++sample) {
                    if (direction == 0) ck(cudaMemsetAsync(card.dst, 0xA7, bytes, card.stream), "poison H2D");
                    else std::memset(scratch, 0xA7, bytes);
                    ck(cudaStreamSynchronize(card.stream), "before single copy");
                    auto start = Clock::now();
                    for (int r = 0; r < reps; ++r) {
                        if (direction == 0) ck(cudaMemcpyAsync(card.dst, host, bytes, cudaMemcpyHostToDevice, card.stream), "H2D");
                        else ck(cudaMemcpyAsync(scratch, card.src, bytes, cudaMemcpyDeviceToHost, card.stream), "D2H");
                    }
                    ck(cudaStreamSynchronize(card.stream), "single copy completion");
                    double us = std::chrono::duration<double,std::micro>(Clock::now()-start).count()/reps;
                    if (direction == 0) verify(card, scratch, bytes);
                    else if (std::memcmp(scratch, card.expected.data(), bytes)) return 2;
                    if (sample >= 0) samples.push_back(us);
                }
                report(direction == 0 ? "pinned_H2D" : "pinned_D2H", direction == 0 ? "host" : card.pci,
                       direction == 0 ? card.pci : "host", bytes, reps, samples, card);
            }
        }
    }
    // Both directions of the actual Ampere pair. cudaMemcpyPeerAsync is an API
    // copy and may fall back without direct peer mappings; never label it direct.
    for (auto pair : {std::pair<int,int>{0,1}, {1,0}}) {
        Card &source = cards[pair.first], &dest = cards[pair.second];
        int peer = -1;
        ck(cudaDeviceCanAccessPeer(&peer, dest.id, source.id), "peer capability");
        std::printf("{\"type\":\"peer_capability\",\"source\":\"%s\",\"destination\":\"%s\",\"can_access\":%d}\n", source.pci, dest.pci, peer);
        auto original = dest.expected;
        dest.expected = source.expected;
        for (size_t bytes : {size_t(4096), size_t(65536), capacity}) {
            int reps = bytes == capacity ? 3 : 8;
            for (int staged = 0; staged < 2; ++staged) {
                std::vector<double> samples;
                for (int sample = -1; sample < 5; ++sample) {
                    ck(cudaSetDevice(dest.id), "peer destination");
                    ck(cudaMemsetAsync(dest.dst, 0xA7, bytes, dest.stream), "poison peer destination");
                    ck(cudaStreamSynchronize(dest.stream), "poison completion");
                    auto start = Clock::now();
                    for (int r = 0; r < reps; ++r) {
                        if (!staged) {
                            ck(cudaMemcpyPeerAsync(dest.dst, dest.id, source.src, source.id, bytes, dest.stream), "runtime peer copy");
                        } else {
                            ck(cudaSetDevice(source.id), "staged source");
                            ck(cudaMemcpyAsync(scratch, source.src, bytes, cudaMemcpyDeviceToHost, source.stream), "staged D2H");
                            ck(cudaStreamSynchronize(source.stream), "staged D2H complete");
                            ck(cudaSetDevice(dest.id), "staged destination");
                            ck(cudaMemcpyAsync(dest.dst, scratch, bytes, cudaMemcpyHostToDevice, dest.stream), "staged H2D");
                            ck(cudaStreamSynchronize(dest.stream), "staged H2D complete");
                        }
                    }
                    ck(cudaStreamSynchronize(dest.stream), "peer copy completion");
                    double us = std::chrono::duration<double,std::micro>(Clock::now()-start).count()/reps;
                    verify(dest, host, bytes);
                    // Ensure transfer did not alter or alias the remote source.
                    ck(cudaSetDevice(source.id), "check remote source");
                    ck(cudaMemcpy(host, source.src, bytes, cudaMemcpyDeviceToHost), "source readback");
                    if (std::memcmp(host, source.expected.data(), bytes)) return 2;
                    ck(cudaSetDevice(dest.id), "restore destination");
                    if (sample >= 0) samples.push_back(us);
                }
                report(staged ? "explicit_host_staged" : (enable_peer ? "runtime_peer_copy_enabled" : "runtime_peer_copy_unenabled"), source.pci, dest.pci, bytes, reps, samples, dest);
            }
        }
        dest.expected = std::move(original);
    }
    if (enable_peer) {
        for (auto pair : {std::pair<int,int>{0,1}, {1,0}}) {
            ck(cudaSetDevice(cards[pair.first].id), "peer cleanup context");
            ck(cudaDeviceDisablePeerAccess(cards[pair.second].id), "disable process peer mapping");
        }
    }
    for (auto &card : cards) {
        ck(cudaSetDevice(card.id), "cleanup device");
        ck(cudaFree(card.src), "free source"); ck(cudaFree(card.dst), "free destination");
        ck(cudaStreamDestroy(card.stream), "destroy stream");
    }
    ck(cudaFreeHost(host), "free host"); ck(cudaFreeHost(scratch), "free scratch");
    return 0;
}
