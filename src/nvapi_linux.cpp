// SPDX-License-Identifier: MIT
#include "nvapi_linux.h"

#ifdef __linux__

#include <dlfcn.h>

#include <cstddef>
#include <mutex>

namespace {

// NVAPI is a C API.  Only the handful of declarations needed here are
// reproduced; they mirror the official nvapi.h and the layout LACT uses.
using NvPhysicalGpuHandle = void *;
using NvU32 = uint32_t;
using NvU8 = uint8_t;
using NvAPI_Status = int;

constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvU32 NVAPI_MAX_PHYSICAL_GPUS = 64;

// Documented entry points, looked up through NvAPI_QueryInterface.
constexpr NvU32 QUERY_INITIALIZE = 0x0150e828;
constexpr NvU32 QUERY_ENUM_PHYSICAL_GPUS = 0xe5ac921f;
constexpr NvU32 QUERY_GPU_GET_BUS_ID = 0x1be0b8e5;

// Undocumented entry point: NvAPI_Client_VoltRailsGetStatus.  Same id LACT
// uses (lact-daemon/src/server/gpu_controller/nvidia/nvapi.rs).  Unknown to
// NVML and to nvidia-smi, which is why MangoHud never had it.
constexpr NvU32 QUERY_CLIENT_VOLT_RAILS_GET_STATUS = 0x465f9bcf;

// Matches ClientVoltRailStatusV1 / ClientVoltRailsStatusV1 (version 1).
struct ClientVoltRailStatusV1 {
    NvU32 rail_id;
    NvU32 current_voltage_uv;
    NvU8 reserved[32];
};

struct ClientVoltRailsStatusV1 {
    NvU32 version;
    NvU8 reserved[32];
    ClientVoltRailStatusV1 rails[1];
};

using NvAPI_QueryInterface_t = void *(*)(NvU32 id);
using NvAPI_Initialize_t = NvAPI_Status (*)();
using NvAPI_EnumPhysicalGPUs_t = NvAPI_Status (*)(NvPhysicalGpuHandle *handles, NvU32 *count);
using NvAPI_GPU_GetBusId_t = NvAPI_Status (*)(NvPhysicalGpuHandle handle, NvU32 *bus_id);
using NvAPI_Client_VoltRailsGetStatus_t = NvAPI_Status (*)(NvPhysicalGpuHandle handle,
                                                           ClientVoltRailsStatusV1 *status);

// NVAPI expects the version field to carry the size of the struct passed in.
constexpr NvU32 make_version(size_t struct_size, NvU32 version)
{
    return static_cast<NvU32>(struct_size) | (version << 16);
}

struct NvapiState {
    bool loaded = false;
    NvAPI_Initialize_t initialize = nullptr;
    NvAPI_EnumPhysicalGPUs_t enum_physical_gpus = nullptr;
    NvAPI_GPU_GetBusId_t get_bus_id = nullptr;
    NvAPI_Client_VoltRailsGetStatus_t volt_rails_get_status = nullptr;

    // The HUD polls the voltage every few milliseconds, so remember which
    // physical GPU belongs to which bus instead of re-enumerating each time.
    bool handle_valid = false;
    NvU32 handle_bus = 0;
    NvPhysicalGpuHandle handle = nullptr;
};

NvapiState state;
std::once_flag init_once;

template <typename Fn>
Fn resolve(NvAPI_QueryInterface_t query_interface, NvU32 id)
{
    return reinterpret_cast<Fn>(query_interface(id));
}

void init()
{
    void *library = dlopen("libnvidia-api.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!library)
        return;

    auto query_interface =
        reinterpret_cast<NvAPI_QueryInterface_t>(dlsym(library, "nvapi_QueryInterface"));
    if (!query_interface)
        return;

    state.initialize = resolve<NvAPI_Initialize_t>(query_interface, QUERY_INITIALIZE);
    state.enum_physical_gpus =
        resolve<NvAPI_EnumPhysicalGPUs_t>(query_interface, QUERY_ENUM_PHYSICAL_GPUS);
    state.get_bus_id = resolve<NvAPI_GPU_GetBusId_t>(query_interface, QUERY_GPU_GET_BUS_ID);
    state.volt_rails_get_status = resolve<NvAPI_Client_VoltRailsGetStatus_t>(
        query_interface, QUERY_CLIENT_VOLT_RAILS_GET_STATUS);

    if (!state.initialize || !state.enum_physical_gpus || !state.get_bus_id ||
        !state.volt_rails_get_status)
        return;

    if (state.initialize() != NVAPI_OK)
        return;

    state.loaded = true;
}

// Resolves the physical GPU that lives on `bus_number` (0 = the first one).
NvPhysicalGpuHandle find_handle(NvU32 bus_number)
{
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 count = 0;
    if (state.enum_physical_gpus(handles, &count) != NVAPI_OK || count == 0)
        return nullptr;

    if (bus_number == 0)
        return handles[0];

    for (NvU32 i = 0; i < count && i < NVAPI_MAX_PHYSICAL_GPUS; i++) {
        NvU32 bus_id = 0;
        if (state.get_bus_id(handles[i], &bus_id) == NVAPI_OK && bus_id == bus_number)
            return handles[i];
    }

    return nullptr;
}

NvPhysicalGpuHandle find_handle_cached(NvU32 bus_number)
{
    if (state.handle_valid && state.handle_bus == bus_number)
        return state.handle;

    NvPhysicalGpuHandle handle = find_handle(bus_number);
    if (!handle)
        return nullptr;

    state.handle = handle;
    state.handle_bus = bus_number;
    state.handle_valid = true;
    return handle;
}

} // namespace

namespace nvapi_linux {

bool available()
{
    std::call_once(init_once, init);
    return state.loaded;
}

bool get_core_voltage_mv(unsigned int bus_number, int &out_mv)
{
    if (!available())
        return false;

    NvPhysicalGpuHandle handle = find_handle_cached(bus_number);
    if (!handle)
        return false;

    ClientVoltRailsStatusV1 status = {};
    status.version = make_version(sizeof(status), 1);

    if (state.volt_rails_get_status(handle, &status) != NVAPI_OK)
        return false;

    out_mv = static_cast<int>(status.rails[0].current_voltage_uv / 1000);
    return true;
}

} // namespace nvapi_linux

#endif // __linux__
