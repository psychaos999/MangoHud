// SPDX-License-Identifier: MIT
//
// GPU core voltage on NVIDIA GPUs, via NVAPI.
//
// The NVML interface used for the rest of the NVIDIA metrics does not expose
// GPU core voltage, so `gpu_voltage` used to report 0 on every NVIDIA card.
// NVAPI does expose it through the undocumented "client volt rails" query -
// the same interface LACT uses.  That query id is private, hence everything
// here is resolved dynamically at runtime and any failure is reported as
// "unavailable" rather than being fatal.

#pragma once

#ifdef __linux__

#include <cstdint>

namespace nvapi_linux {

// Lazily loads libnvidia-api.so.1 and resolves the entry points we need.
// Returns false - and keeps returning false - when NVAPI or the private
// voltage query is unavailable, e.g. when no NVIDIA driver is present.
bool available();

// Reads the core voltage of the physical GPU sitting on PCI bus `bus_number`
// (0 selects the first physical GPU) and stores it as millivolts in `out_mv`.
// Returns false when the value could not be read.
bool get_core_voltage_mv(unsigned int bus_number, int &out_mv);

} // namespace nvapi_linux

#endif // __linux__
