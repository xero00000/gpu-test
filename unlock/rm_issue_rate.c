// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
/*
 * Read NVIDIA RM's cached SM issue-rate values and feature gates.
 *
 * This program uses only public GET controls from NVIDIA's open driver. The
 * minimal ABI definitions below match open-gpu-kernel-modules 610.43.03 and
 * 610.57.04.
 * No control in this file changes GPU state.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NV_ALIGN_BYTES(size) __attribute__((aligned(size)))
#define NV_DECLARE_ALIGNED(type_and_name, alignment) \
    type_and_name __attribute__((aligned(alignment)))

typedef uint8_t NvU8;
typedef uint8_t NvBool;
typedef uint32_t NvU32;
typedef uint32_t NvV32;
typedef uint32_t NvHandle;
typedef uint64_t NvU64;
typedef void *NvP64;

#define NV_IOCTL_MAGIC 'F'
#define NV_IOCTL_BASE 200
#define NV_ESC_REGISTER_FD (NV_IOCTL_BASE + 1)
#define NV_ESC_RM_CONTROL 0x2a
#define NV_ESC_RM_ALLOC 0x2b

#define NV01_DEVICE_0 0x80U
#define NV20_SUBDEVICE_0 0x2080U

#define NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER 0x20801230U
#define NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER_V2 0x2080123cU
#define NV2080_CTRL_CMD_GR_GET_SM_ISSUE_THROTTLE_CTRL 0x2080123dU
#define NV2080_CTRL_CMD_GPU_GET_INFO_V2 0x20800102U
#define NV2080_CTRL_CMD_GR_GET_INFO 0x20801201U
#define NV2080_CTRL_CMD_GR_GET_ENGINE_CONTEXT_PROPERTIES 0x2080122dU
#define NV2080_CTRL_CMD_GPU_GET_ENGINES_V2 0x20800170U
#define NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST 0x20800124U
#define NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO 0x208001b0U
#define NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE 0x20801112U

#define NV2080_CTRL_GPU_INFO_INDEX_NETLIST_REV0 0x12U
#define NV2080_CTRL_GPU_INFO_INDEX_NETLIST_REV1 0x13U
#define NV2080_CTRL_GPU_INFO_INDEX_DISPLAY_ENABLED 0x34U
#define NV2080_CTRL_GPU_INFO_INDEX_CMP_SKU 0x3cU
#define NV2080_CTRL_GR_INFO_INDEX_GPU_CORE_COUNT 0x1dU
#define NV2080_CTRL_GR_INFO_INDEX_RT_CORE_COUNT 0x22U
#define NV2080_CTRL_GR_INFO_INDEX_TENSOR_CORE_COUNT 0x23U
#define NV2080_CTRL_GR_INFO_INDEX_GFX_CAPABILITIES 0x34U

#define NV2080_ENGINE_TYPE_GRAPHICS 0x01U
#define NV2080_ENGINE_TYPE_NVDEC0 0x13U
#define NV2080_ENGINE_TYPE_NVENC0 0x1bU
#define NV2080_ENGINE_TYPE_DPU 0x28U

#define NV2080_CTRL_GR_SM_ISSUE_RATE_MODIFIER_V2_MAX_LIST_SIZE 0xffU
#define NV2080_CTRL_GR_SM_ISSUE_THROTTLE_CTRL_MAX_LIST_SIZE 0xffU
#define NV2080_CTRL_GPU_INFO_MAX_LIST_SIZE 72U
#define NV2080_CTRL_GPU_MAX_CONSTRUCTED_FALCONS 0x40U
#define NV2080_GPU_MAX_ENGINES_LIST_SIZE 0x54U
#define NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES 32U
#define NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_DATA_TYPES 16U
#define NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_PBDMA 2U
#define NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_NAME_LEN 16U
#define NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT 0x1aU
#define CMP_ENGINE_CLASS_CAPACITY 200U

typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32 hClass;
    NvP64 pAllocParms NV_ALIGN_BYTES(8);
    NvU32 paramsSize;
    NvV32 status;
} NVOS21_PARAMETERS;

typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32 hClass;
    NvP64 pAllocParms NV_ALIGN_BYTES(8);
    NvP64 pRightsRequested NV_ALIGN_BYTES(8);
    NvU32 paramsSize;
    NvU32 flags;
    NvV32 status;
} NVOS64_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hObject;
    NvV32 cmd;
    NvU32 flags;
    NvP64 params NV_ALIGN_BYTES(8);
    NvU32 paramsSize;
    NvV32 status;
} NVOS54_PARAMETERS;

typedef struct {
    NvU32 deviceId;
    NvHandle hClientShare;
    NvHandle hTargetClient;
    NvHandle hTargetDevice;
    NvV32 flags;
    NV_DECLARE_ALIGNED(NvU64 vaSpaceSize, 8);
    NV_DECLARE_ALIGNED(NvU64 vaStartInternal, 8);
    NV_DECLARE_ALIGNED(NvU64 vaLimitInternal, 8);
    NvV32 vaMode;
} NV0080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 subDeviceId;
} NV2080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 flags;
    NV_DECLARE_ALIGNED(NvU64 route, 8);
} NV2080_CTRL_GR_ROUTE_INFO;

typedef struct {
    NV_DECLARE_ALIGNED(NV2080_CTRL_GR_ROUTE_INFO grRouteInfo, 8);
    NvU8 imla0;
    NvU8 fmla16;
    NvU8 dp;
    NvU8 fmla32;
    NvU8 ffma;
    NvU8 imla1;
    NvU8 imla2;
    NvU8 imla3;
    NvU8 imla4;
} NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS;

typedef struct {
    NvU32 index;
    NvU32 data;
} NVXXXX_CTRL_XXX_INFO;

typedef struct {
    NvU32 gpuInfoListSize;
    NV_DECLARE_ALIGNED(NvP64 gpuInfoList, 8);
} NV2080_CTRL_GPU_GET_INFO_PARAMS;

typedef struct {
    NvU32 gpuInfoListSize;
    NVXXXX_CTRL_XXX_INFO
        gpuInfoList[NV2080_CTRL_GPU_INFO_MAX_LIST_SIZE];
} NV2080_CTRL_GPU_GET_INFO_V2_PARAMS;

typedef struct {
    NvU32 grInfoListSize;
    NV_DECLARE_ALIGNED(NvP64 grInfoList, 8);
    NV_DECLARE_ALIGNED(NV2080_CTRL_GR_ROUTE_INFO grRouteInfo, 8);
} NV2080_CTRL_GR_GET_INFO_PARAMS;

typedef struct {
    NvU32 engineType;
    NvU32 numClasses;
    NV_DECLARE_ALIGNED(NvP64 classList, 8);
} NV2080_CTRL_GPU_GET_ENGINE_CLASSLIST_PARAMS;

typedef struct {
    NvU32 engineCount;
    NvU32 engineList[NV2080_GPU_MAX_ENGINES_LIST_SIZE];
} NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS;

typedef struct {
    NV_DECLARE_ALIGNED(NV2080_CTRL_GR_ROUTE_INFO grRouteInfo, 8);
    NvU32 engineId;
    NvU32 alignment;
    NvU32 size;
    NvBool bInfoPopulated;
} NV2080_CTRL_GR_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS;

typedef struct {
    NvU32 engineData[NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_DATA_TYPES];
    NvU32 pbdmaIds[NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_PBDMA];
    NvU32 pbdmaFaultIds[NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_PBDMA];
    NvU32 numPbdmas;
    char engineName[NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_NAME_LEN];
} NV2080_CTRL_FIFO_DEVICE_ENTRY;

typedef struct {
    NvU32 baseIndex;
    NvU32 numEntries;
    NvBool bMore;
    NV2080_CTRL_FIFO_DEVICE_ENTRY
        entries[NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES];
} NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS;

typedef struct {
    NvU32 engDesc;
    NvU32 ctxAttr;
    NvU32 ctxBufferSize;
    NvU32 addrSpaceList;
    NvU32 registerBase;
} NV2080_CTRL_GPU_CONSTRUCTED_FALCON_INFO;

typedef struct {
    NvU32 numConstructedFalcons;
    NV2080_CTRL_GPU_CONSTRUCTED_FALCON_INFO
        constructedFalconsTable[NV2080_CTRL_GPU_MAX_CONSTRUCTED_FALCONS];
} NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS;

typedef struct {
    NvU32 smIssueRateModifierListSize;
    NVXXXX_CTRL_XXX_INFO
        smIssueRateModifierList[NV2080_CTRL_GR_SM_ISSUE_RATE_MODIFIER_V2_MAX_LIST_SIZE];
} NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_V2_PARAMS;

typedef struct {
    NvU32 smIssueThrottleCtrlListSize;
    NVXXXX_CTRL_XXX_INFO
        smIssueThrottleCtrlList[NV2080_CTRL_GR_SM_ISSUE_THROTTLE_CTRL_MAX_LIST_SIZE];
} NV2080_CTRL_GR_GET_SM_ISSUE_THROTTLE_CTRL_PARAMS;

_Static_assert(sizeof(NVOS21_PARAMETERS) == 32, "NVOS21 ABI size changed");
_Static_assert(sizeof(NVOS64_PARAMETERS) == 48, "NVOS64 ABI size changed");
_Static_assert(sizeof(NVOS54_PARAMETERS) == 32, "NVOS54 ABI size changed");
_Static_assert(sizeof(NV0080_ALLOC_PARAMETERS) == 56, "NV0080 ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GR_ROUTE_INFO) == 16, "GR route ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS) == 32,
               "SM issue-rate ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GPU_GET_INFO_PARAMS) == 16,
               "GPU info ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GPU_GET_INFO_V2_PARAMS) == 580,
               "GPU info V2 ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GR_GET_INFO_PARAMS) == 32,
               "GR info ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GPU_GET_ENGINE_CLASSLIST_PARAMS) == 16,
               "engine class-list ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS) == 340,
               "engine-list ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GR_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS) == 32,
               "engine context-properties ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_FIFO_DEVICE_ENTRY) == 100,
               "device-info entry ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS) == 3212,
               "device-info table ABI size changed");
_Static_assert(sizeof(NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS) == 1284,
               "constructed Falcon ABI size changed");

typedef struct {
    int os_error;
    NvU32 rm_status;
} ControlResult;

typedef struct {
    const char *name;
    NvU32 engine_type;
    NvU32 num_classes;
    NvU32 classes[CMP_ENGINE_CLASS_CAPACITY];
    ControlResult result;
} EngineClassQuery;

static const char *const v1_names[] = {
    "IMLA0", "FMLA16", "DP", "FMLA32", "FFMA",
    "IMLA1", "IMLA2", "IMLA3", "IMLA4",
};

static const char *const v2_names[] = {
    "FMLA16", "DP", "FMLA32", "FFMA", "IMLA0", "IMLA1", "IMLA2",
    "IMLA3", "IMLA4", "FP16", "FP32", "DFMA", "DMLA",
};

static const char *const throttle_names[] = {"MASK", "CREDIT"};
static const char *const gpu_info_names[] = {
    "NETLIST_REV0",
    "NETLIST_REV1",
    "CMP_SKU",
    "DISPLAY_ENABLED",
};
static const char *const gr_info_names[] = {
    "GPU_CORE_COUNT", "RT_CORE_COUNT", "TENSOR_CORE_COUNT", "GFX_CAPABILITIES",
};

static const char *const context_buffer_names[] = {
    "GRAPHICS", "VLD", "VIDEO", "MPEG", "CAPTURE", "DISPLAY",
    "ENCRYPTION", "POSTPROCESS", "GRAPHICS_ZCULL", "GRAPHICS_PM",
    "COMPUTE_PREEMPT", "GRAPHICS_PREEMPT", "GRAPHICS_SPILL",
    "GRAPHICS_PAGEPOOL", "GRAPHICS_BETACB", "GRAPHICS_RTV",
    "GRAPHICS_PATCH", "GRAPHICS_BUNDLE_CB", "GRAPHICS_PAGEPOOL_GLOBAL",
    "GRAPHICS_ATTRIBUTE_CB", "GRAPHICS_RTV_CB_GLOBAL", "GRAPHICS_GFXP_POOL",
    "GRAPHICS_GFXP_CTRL_BLK", "GRAPHICS_FECS_EVENT",
    "GRAPHICS_PRIV_ACCESS_MAP", "GRAPHICS_SETUP",
};

static bool rm_ok(ControlResult result)
{
    return result.os_error == 0 && result.rm_status == 0;
}

static const char *speed_name(NvU32 value, bool dp_field)
{
    static const char *const names[] = {
        "full", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
    };

    if (dp_field)
        return value == 0 ? "full" : (value == 1 ? "reduced" : "unknown");
    return value < (sizeof(names) / sizeof(names[0])) ? names[value] : "unknown";
}

static bool parse_device_index(const char *text, int *device_index)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 127)
        return false;
    *device_index = (int)value;
    return true;
}

static bool register_device_fd(int nvidia_fd, int nvidiactl_fd)
{
    unsigned long request = _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC,
                                 NV_ESC_REGISTER_FD, sizeof(nvidiactl_fd));
    return ioctl(nvidia_fd, request, &nvidiactl_fd) == 0;
}

static bool alloc_client(int nvidiactl_fd, NvHandle *client)
{
    NVOS21_PARAMETERS request = {0};
    unsigned long ioctl_request = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC,
                                         NVOS21_PARAMETERS);

    if (ioctl(nvidiactl_fd, ioctl_request, &request) != 0) {
        fprintf(stderr, "RM client allocation ioctl failed: %s\n", strerror(errno));
        return false;
    }
    if (request.status != 0) {
        fprintf(stderr, "RM client allocation failed: 0x%08x\n", request.status);
        return false;
    }
    *client = request.hObjectNew;
    return true;
}

static bool alloc_device(int nvidiactl_fd, NvHandle client, int device_index,
                         NvHandle *device)
{
    NV0080_ALLOC_PARAMETERS params = {0};
    NVOS64_PARAMETERS request = {0};
    unsigned long ioctl_request = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC,
                                         NVOS64_PARAMETERS);

    params.deviceId = (NvU32)device_index;
    request.hRoot = client;
    request.hObjectParent = client;
    request.hClass = NV01_DEVICE_0;
    request.pAllocParms = &params;
    request.paramsSize = sizeof(params);

    if (ioctl(nvidiactl_fd, ioctl_request, &request) != 0) {
        fprintf(stderr, "RM device allocation ioctl failed: %s\n", strerror(errno));
        return false;
    }
    if (request.status != 0) {
        fprintf(stderr, "RM device allocation failed: 0x%08x\n", request.status);
        return false;
    }
    *device = request.hObjectNew;
    return true;
}

static bool alloc_subdevice(int nvidiactl_fd, NvHandle client, NvHandle device,
                            NvHandle *subdevice)
{
    NV2080_ALLOC_PARAMETERS params = {0};
    NVOS64_PARAMETERS request = {0};
    unsigned long ioctl_request = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC,
                                         NVOS64_PARAMETERS);

    request.hRoot = client;
    request.hObjectParent = device;
    request.hClass = NV20_SUBDEVICE_0;
    request.pAllocParms = &params;
    request.paramsSize = sizeof(params);

    if (ioctl(nvidiactl_fd, ioctl_request, &request) != 0) {
        fprintf(stderr, "RM subdevice allocation ioctl failed: %s\n", strerror(errno));
        return false;
    }
    if (request.status != 0) {
        fprintf(stderr, "RM subdevice allocation failed: 0x%08x\n", request.status);
        return false;
    }
    *subdevice = request.hObjectNew;
    return true;
}

static ControlResult rm_control(int nvidiactl_fd, NvHandle client,
                                NvHandle subdevice, NvU32 command, void *params,
                                NvU32 params_size)
{
    NVOS54_PARAMETERS request = {0};
    unsigned long ioctl_request = _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL,
                                         NVOS54_PARAMETERS);
    ControlResult result = {0};

    request.hClient = client;
    request.hObject = subdevice;
    request.cmd = command;
    request.params = params;
    request.paramsSize = params_size;

    if (ioctl(nvidiactl_fd, ioctl_request, &request) != 0)
        result.os_error = errno;
    result.rm_status = request.status;
    return result;
}

static void query_engine_classes(int nvidiactl_fd, NvHandle client,
                                 NvHandle subdevice, EngineClassQuery *query)
{
    NV2080_CTRL_GPU_GET_ENGINE_CLASSLIST_PARAMS params = {0};

    params.engineType = query->engine_type;
    params.numClasses = CMP_ENGINE_CLASS_CAPACITY;
    params.classList = query->classes;
    query->result = rm_control(
        nvidiactl_fd, client, subdevice,
        NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST, &params, sizeof(params));
    query->num_classes = rm_ok(query->result) ? params.numClasses : 0;
}

static void print_result_header(const char *name, NvU32 command,
                                ControlResult result)
{
    printf("    \"%s\": {\n", name);
    printf("      \"command\": \"0x%08x\",\n", command);
    printf("      \"os_error\": %d,\n", result.os_error);
    printf("      \"rm_status\": \"0x%08x\",\n", result.rm_status);
}

static void print_v1(ControlResult result,
                     const NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS *params)
{
    const NvU8 values[] = {
        params->imla0, params->fmla16, params->dp, params->fmla32, params->ffma,
        params->imla1, params->imla2, params->imla3, params->imla4,
    };

    print_result_header("v1", NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER,
                        result);
    printf("      \"values\": [");
    if (rm_ok(result)) {
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            printf("%s\n        {\"name\": \"%s\", \"raw\": %u, "
                   "\"speed\": \"%s\"}",
                   i == 0 ? "" : ",", v1_names[i], values[i],
                   speed_name(values[i], i == 2));
        }
        printf("\n      ");
    }
    printf("]\n    },\n");
}

static void print_info_list(const char *name, NvU32 command, ControlResult result,
                            NvU32 count, const NVXXXX_CTRL_XXX_INFO *items,
                            bool v2_fields, bool print_speed,
                            bool trailing_comma)
{
    print_result_header(name, command, result);
    printf("      \"values\": [");
    if (rm_ok(result)) {
        NvU32 safe_count = count > 0xffU ? 0xffU : count;
        for (NvU32 i = 0; i < safe_count; ++i) {
            const char *field;
            if (v2_fields) {
                field = items[i].index <
                                (sizeof(v2_names) / sizeof(v2_names[0]))
                            ? v2_names[items[i].index]
                            : "UNKNOWN";
            } else {
                field = items[i].index <
                                (sizeof(throttle_names) /
                                 sizeof(throttle_names[0]))
                            ? throttle_names[items[i].index]
                            : "UNKNOWN";
            }
            printf("%s\n        {\"index\": %u, \"name\": \"%s\", "
                   "\"raw\": %u",
                   i == 0 ? "" : ",", items[i].index, field, items[i].data);
            if (print_speed)
                printf(", \"speed\": \"%s\"",
                       speed_name(items[i].data, items[i].index == 1));
            printf("}");
        }
        if (safe_count != 0)
            printf("\n      ");
    }
    printf("]\n    }%s\n", trailing_comma ? "," : "");
}

static void print_gpu_info(ControlResult result,
                           const NVXXXX_CTRL_XXX_INFO *items)
{
    print_result_header("gpu_info", NV2080_CTRL_CMD_GPU_GET_INFO_V2, result);
    printf("      \"values\": [");
    if (rm_ok(result)) {
        for (size_t i = 0;
             i < sizeof(gpu_info_names) / sizeof(gpu_info_names[0]); ++i) {
            printf("%s\n        {\"index\": %u, \"name\": \"%s\", "
                   "\"raw\": %u, \"set\": %s}",
                   i == 0 ? "" : ",", items[i].index, gpu_info_names[i],
                   items[i].data, items[i].data != 0 ? "true" : "false");
        }
        printf("\n      ");
    }
    printf("]\n    },\n");
}

static void print_gr_info(ControlResult result,
                          const NVXXXX_CTRL_XXX_INFO *items)
{
    NvU32 capabilities = items[3].data;

    print_result_header("gr_info", NV2080_CTRL_CMD_GR_GET_INFO, result);
    printf("      \"values\": [");
    if (rm_ok(result)) {
        for (size_t i = 0; i < 4; ++i) {
            printf("%s\n        {\"index\": %u, \"name\": \"%s\", "
                   "\"raw\": %u}",
                   i == 0 ? "" : ",", items[i].index, gr_info_names[i],
                   items[i].data);
        }
        printf("\n      ");
    }
    printf("],\n");
    if (rm_ok(result)) {
        printf("      \"gfx_capabilities\": {\"2d\": %s, \"3d\": %s, "
               "\"compute\": %s, \"i2m\": %s}\n",
               (capabilities & 0x1U) != 0 ? "true" : "false",
               (capabilities & 0x2U) != 0 ? "true" : "false",
               (capabilities & 0x4U) != 0 ? "true" : "false",
               (capabilities & 0x8U) != 0 ? "true" : "false");
    } else {
        printf("      \"gfx_capabilities\": null\n");
    }
    printf("    },\n");
}

static void print_context_buffers(
    const ControlResult
        results[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT],
    const NV2080_CTRL_GR_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS
        params[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT])
{
    printf("    \"gr_context_buffers\": {\n");
    printf("      \"command\": \"0x%08x\",\n",
           NV2080_CTRL_CMD_GR_GET_ENGINE_CONTEXT_PROPERTIES);
    printf("      \"entries\": [");
    for (NvU32 i = 0;
         i < NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT;
         ++i) {
        printf("%s\n        {\"engine_id\": %u, \"name\": \"%s\", "
               "\"os_error\": %d, \"rm_status\": \"0x%08x\", "
               "\"alignment\": %u, \"size\": %u, \"populated\": %s}",
               i == 0 ? "" : ",", i, context_buffer_names[i],
               results[i].os_error, results[i].rm_status,
               params[i].alignment, params[i].size,
               params[i].bInfoPopulated ? "true" : "false");
    }
    printf("\n      ]\n    },\n");
}

static void print_engine_classes(const EngineClassQuery *query,
                                 bool trailing_comma)
{
    print_result_header(query->name,
                        NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST,
                        query->result);
    printf("      \"engine_type\": \"0x%08x\",\n", query->engine_type);
    printf("      \"capacity\": %u,\n", CMP_ENGINE_CLASS_CAPACITY);
    printf("      \"num_classes\": %u,\n", query->num_classes);
    printf("      \"classes\": [");
    if (rm_ok(query->result)) {
        NvU32 safe_count = query->num_classes > CMP_ENGINE_CLASS_CAPACITY
                               ? CMP_ENGINE_CLASS_CAPACITY
                               : query->num_classes;
        for (NvU32 i = 0; i < safe_count; ++i)
            printf("%s\"0x%08x\"", i == 0 ? "" : ", ", query->classes[i]);
    }
    printf("]\n    }%s\n", trailing_comma ? "," : "");
}

static void print_constructed_falcons(
    ControlResult result,
    const NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS *params)
{
    print_result_header("constructed_falcons",
                        NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO,
                        result);
    printf("      \"num_falcons\": %u,\n", params->numConstructedFalcons);
    printf("      \"falcons\": [");
    if (rm_ok(result)) {
        NvU32 count = params->numConstructedFalcons;
        if (count > NV2080_CTRL_GPU_MAX_CONSTRUCTED_FALCONS)
            count = NV2080_CTRL_GPU_MAX_CONSTRUCTED_FALCONS;
        for (NvU32 i = 0; i < count; ++i) {
            const NV2080_CTRL_GPU_CONSTRUCTED_FALCON_INFO *falcon =
                &params->constructedFalconsTable[i];
            printf("%s\n        {\"eng_desc\": \"0x%08x\", "
                   "\"class_id\": \"0x%06x\", \"instance\": %u, "
                   "\"ctx_attr\": \"0x%08x\", "
                   "\"ctx_buffer_size\": %u, "
                   "\"addr_space_list\": \"0x%08x\", "
                   "\"register_base\": \"0x%08x\"}",
                   i == 0 ? "" : ",", falcon->engDesc,
                   falcon->engDesc >> 8, falcon->engDesc & 0xffU,
                   falcon->ctxAttr, falcon->ctxBufferSize,
                   falcon->addrSpaceList, falcon->registerBase);
        }
        if (count != 0)
            printf("\n      ");
    }
    printf("]\n    },\n");
}

static const char *engine_type_name(NvU32 engine_type)
{
    switch (engine_type) {
    case NV2080_ENGINE_TYPE_GRAPHICS:
        return "GRAPHICS";
    case NV2080_ENGINE_TYPE_NVDEC0:
        return "NVDEC0";
    case NV2080_ENGINE_TYPE_NVENC0:
        return "NVENC0";
    case NV2080_ENGINE_TYPE_DPU:
        return "DPU";
    default:
        return "OTHER";
    }
}

static void print_engine_list(
    ControlResult result,
    const NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS *params)
{
    print_result_header("engines_v2", NV2080_CTRL_CMD_GPU_GET_ENGINES_V2,
                        result);
    printf("      \"engine_count\": %u,\n", params->engineCount);
    printf("      \"engines\": [");
    if (rm_ok(result)) {
        NvU32 count = params->engineCount;
        if (count > NV2080_GPU_MAX_ENGINES_LIST_SIZE)
            count = NV2080_GPU_MAX_ENGINES_LIST_SIZE;
        for (NvU32 i = 0; i < count; ++i) {
            NvU32 engine_type = params->engineList[i];
            printf("%s\n        {\"type\": \"0x%08x\", \"name\": \"%s\"}",
                   i == 0 ? "" : ",", engine_type,
                   engine_type_name(engine_type));
        }
        if (count != 0)
            printf("\n      ");
    }
    printf("]\n    },\n");
}

static void print_device_info_table(
    ControlResult result,
    const NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS *params)
{
    print_result_header("fifo_device_info",
                        NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, result);
    printf("      \"base_index\": %u,\n", params->baseIndex);
    printf("      \"num_entries\": %u,\n", params->numEntries);
    printf("      \"more\": %s,\n", params->bMore ? "true" : "false");
    printf("      \"entries\": [");
    if (rm_ok(result)) {
        NvU32 count = params->numEntries;
        if (count > NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES)
            count = NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES;
        for (NvU32 i = 0; i < count; ++i) {
            const NV2080_CTRL_FIFO_DEVICE_ENTRY *entry = &params->entries[i];
            printf("%s\n        {\"engine_name_hex\": \"", i == 0 ? "" : ",");
            for (size_t j = 0; j < sizeof(entry->engineName); ++j)
                printf("%02x", (unsigned char)entry->engineName[j]);
            printf("\", \"num_pbdmas\": %u, \"engine_data\": [",
                   entry->numPbdmas);
            for (size_t j = 0;
                 j < NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_DATA_TYPES;
                 ++j)
                printf("%s\"0x%08x\"", j == 0 ? "" : ", ",
                       entry->engineData[j]);
            printf("]}");
        }
        if (count != 0)
            printf("\n      ");
    }
    printf("]\n    },\n");
}

static int run_probe(int nvidiactl_fd, NvHandle client, NvHandle subdevice,
                     int device_index)
{
    NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_PARAMS v1 = {0};
    NV2080_CTRL_GR_GET_SM_ISSUE_RATE_MODIFIER_V2_PARAMS v2 = {0};
    NV2080_CTRL_GPU_GET_CONSTRUCTED_FALCON_INFO_PARAMS falcons = {0};
    NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS engines = {0};
    NV2080_CTRL_GR_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS
        context_buffers[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT] = {0};
    ControlResult
        context_buffer_results[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT] = {0};
    NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS device_info = {0};
    NV2080_CTRL_GPU_GET_INFO_V2_PARAMS gpu_info_params = {
        .gpuInfoListSize = 4,
        .gpuInfoList = {
            {NV2080_CTRL_GPU_INFO_INDEX_NETLIST_REV0, 0},
            {NV2080_CTRL_GPU_INFO_INDEX_NETLIST_REV1, 0},
            {NV2080_CTRL_GPU_INFO_INDEX_CMP_SKU, 0},
            {NV2080_CTRL_GPU_INFO_INDEX_DISPLAY_ENABLED, 0},
        },
    };
    NVXXXX_CTRL_XXX_INFO gr_info[] = {
        {NV2080_CTRL_GR_INFO_INDEX_GPU_CORE_COUNT, 0},
        {NV2080_CTRL_GR_INFO_INDEX_RT_CORE_COUNT, 0},
        {NV2080_CTRL_GR_INFO_INDEX_TENSOR_CORE_COUNT, 0},
        {NV2080_CTRL_GR_INFO_INDEX_GFX_CAPABILITIES, 0},
    };
    NV2080_CTRL_GR_GET_INFO_PARAMS gr_info_params = {
        .grInfoListSize = sizeof(gr_info) / sizeof(gr_info[0]),
        .grInfoList = gr_info,
    };
    EngineClassQuery engine_queries[] = {
        {.name = "engine_classes_graphics",
         .engine_type = NV2080_ENGINE_TYPE_GRAPHICS},
        {.name = "engine_classes_nvdec0",
         .engine_type = NV2080_ENGINE_TYPE_NVDEC0},
        {.name = "engine_classes_nvenc0",
         .engine_type = NV2080_ENGINE_TYPE_NVENC0},
        {.name = "engine_classes_dpu", .engine_type = NV2080_ENGINE_TYPE_DPU},
    };
    ControlResult v1_result;
    ControlResult v2_result;
    ControlResult gpu_info_result;
    ControlResult gr_info_result;
    ControlResult falcons_result;
    ControlResult engines_result;
    ControlResult device_info_result;

    v1_result = rm_control(nvidiactl_fd, client, subdevice,
                           NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER, &v1,
                           sizeof(v1));
    v2_result = rm_control(nvidiactl_fd, client, subdevice,
                           NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER_V2, &v2,
                           sizeof(v2));
    gpu_info_result = rm_control(nvidiactl_fd, client, subdevice,
                                 NV2080_CTRL_CMD_GPU_GET_INFO_V2,
                                 &gpu_info_params, sizeof(gpu_info_params));
    gr_info_result = rm_control(nvidiactl_fd, client, subdevice,
                                NV2080_CTRL_CMD_GR_GET_INFO,
                                &gr_info_params, sizeof(gr_info_params));
    falcons_result = rm_control(
        nvidiactl_fd, client, subdevice,
        NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO, &falcons,
        sizeof(falcons));
    engines_result = rm_control(nvidiactl_fd, client, subdevice,
                                NV2080_CTRL_CMD_GPU_GET_ENGINES_V2, &engines,
                                sizeof(engines));
    for (NvU32 i = 0;
         i < NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT;
         ++i) {
        context_buffers[i].engineId = i;
        context_buffer_results[i] = rm_control(
            nvidiactl_fd, client, subdevice,
            NV2080_CTRL_CMD_GR_GET_ENGINE_CONTEXT_PROPERTIES,
            &context_buffers[i], sizeof(context_buffers[i]));
    }
    device_info_result = rm_control(
        nvidiactl_fd, client, subdevice,
        NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, &device_info,
        sizeof(device_info));
    for (size_t i = 0;
         i < sizeof(engine_queries) / sizeof(engine_queries[0]); ++i)
        query_engine_classes(nvidiactl_fd, client, subdevice,
                             &engine_queries[i]);

    printf("{\n");
    printf("  \"mode\": \"NVIDIA RM GET controls only\",\n");
    printf("  \"device_index\": %d,\n", device_index);
    printf("  \"controls\": {\n");
    print_v1(v1_result, &v1);
    print_info_list("v2", NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER_V2,
                    v2_result, v2.smIssueRateModifierListSize,
                    v2.smIssueRateModifierList, true, false, true);
    print_gpu_info(gpu_info_result, gpu_info_params.gpuInfoList);
    print_gr_info(gr_info_result, gr_info);
    print_context_buffers(context_buffer_results, context_buffers);
    print_constructed_falcons(falcons_result, &falcons);
    print_engine_list(engines_result, &engines);
    print_device_info_table(device_info_result, &device_info);
    for (size_t i = 0;
         i < sizeof(engine_queries) / sizeof(engine_queries[0]); ++i) {
        bool trailing = i + 1 < sizeof(engine_queries) / sizeof(engine_queries[0]);
        print_engine_classes(&engine_queries[i], trailing);
    }
    printf("  }\n");
    printf("}\n");

    return rm_ok(v1_result) || rm_ok(v2_result) ? 0 : 1;
}

int main(int argc, char **argv)
{
    int device_index;
    int nvidiactl_fd = -1;
    int nvidia_fd = -1;
    int result = 1;
    char device_path[32];
    NvHandle client = 0;
    NvHandle device = 0;
    NvHandle subdevice = 0;

    if (argc != 2 || !parse_device_index(argv[1], &device_index)) {
        fprintf(stderr, "Usage: %s NVIDIA_DEVICE_INDEX\n", argv[0]);
        return 2;
    }

    nvidiactl_fd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (nvidiactl_fd < 0) {
        fprintf(stderr, "Cannot open /dev/nvidiactl: %s\n", strerror(errno));
        goto cleanup;
    }

    snprintf(device_path, sizeof(device_path), "/dev/nvidia%d", device_index);
    nvidia_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (nvidia_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device_path, strerror(errno));
        goto cleanup;
    }
    if (!register_device_fd(nvidia_fd, nvidiactl_fd)) {
        fprintf(stderr, "NV_ESC_REGISTER_FD failed: %s\n", strerror(errno));
        goto cleanup;
    }
    if (!alloc_client(nvidiactl_fd, &client) ||
        !alloc_device(nvidiactl_fd, client, device_index, &device) ||
        !alloc_subdevice(nvidiactl_fd, client, device, &subdevice))
        goto cleanup;

    result = run_probe(nvidiactl_fd, client, subdevice, device_index);

cleanup:
    if (nvidia_fd >= 0)
        close(nvidia_fd);
    if (nvidiactl_fd >= 0)
        close(nvidiactl_fd);
    return result;
}
