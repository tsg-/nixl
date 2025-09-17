/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2025 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_TOPOLOGY_H
#define NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_TOPOLOGY_H

#include "libfabric_common.h"
#include "nixl.h"
#include <hwloc.h>
#include <map>

/**
 * @brief Topology discovery and management for libfabric devices (EFA, verbs, sockets, etc.)
 *
 * Automatically discovers system topology using hwloc and maps GPUs to libfabric devices
 * based on PCIe proximity for optimal performance. Falls back gracefully for non-EFA devices.
 */
class nixlLibfabricTopology {
private:
    // GPU to libfabric device mapping: GPU 0→[efa0,efa1], GPU 1→[efa2,efa3], etc.
    std::map<int, std::vector<std::string>> gpu_to_libfabric_devices;

    // NUMA to libfabric device mapping: NUMA 0→[efa0-7], NUMA 1→[efa8-15]
    std::map<int, std::vector<std::string>> numa_to_libfabric_devices;

    // All available libfabric devices discovered on this system
    std::vector<std::string> all_libfabric_devices;

    // System information
    int num_gpus;
    int num_numa_nodes;
    int num_libfabric_devices;

    // Discovery state
    bool topology_discovered;

    // hwloc topology handle
    hwloc_topology_t hwloc_topology;

    // PCIe to Libfabric device mapping
    std::map<std::string, std::string> pcie_to_libfabric_map;
    std::map<std::string, std::string> libfabric_to_pcie_map;

    // Helper methods
    nixl_status_t
    discoverLibfabricDevices();
    nixl_status_t
    discoverTopology();

    // hwloc-based discovery methods
    nixl_status_t
    initHwlocTopology();
    nixl_status_t
    discoverHwlocTopology();
    nixl_status_t
    buildPcieToLibfabricMapping();
    nixl_status_t
    discoverGpusWithHwloc();
    nixl_status_t
    discoverLibfabricDevicesWithHwloc();
    nixl_status_t
    buildGpuToLibfabricMapping();
    void
    cleanupHwlocTopology();

    // Data structures for NIXL topology-aware grouping algorithm
    struct NicInfo {
        std::string libfabric_name;
        hwloc_obj_t hwloc_node;
        uint16_t domain_id;
        uint8_t bus_id;
        uint8_t device_id;
        uint8_t function_id;
    };

    struct GpuInfo {
        hwloc_obj_t hwloc_node;
        uint16_t domain_id;
        uint8_t bus_id;
        uint8_t device_id;
        uint8_t function_id;
    };

    struct NicGroup {
        std::vector<NicInfo> nics;
        GpuInfo closest_gpu;
        hwloc_obj_t common_ancestor;
        bool has_gpu;
    };

    // NIXL topology-aware grouping algorithm methods
    nixl_status_t
    buildTopologyAwareGrouping();
    nixl_status_t
    buildFallbackMapping();
    nixl_status_t
    buildFallbackNumaMapping();
    nixl_status_t
    groupNicsWithGpus(const std::vector<NicInfo> &discovered_nics,
                      const std::vector<GpuInfo> &discovered_gpus,
                      std::vector<NicGroup> &nic_groups);

    // hwloc helper methods
    std::string
    getPcieAddressFromHwlocObj(hwloc_obj_t obj) const;
    bool
    isNvidiaGpu(hwloc_obj_t obj) const;
    bool
    isEfaDevice(hwloc_obj_t obj) const;

public:
    nixlLibfabricTopology(); // Automatically discovers topology, throws on failure
    ~nixlLibfabricTopology();
    // GPU-based queries
    std::vector<std::string>
    getLibfabricDevicesForGpu(int gpu_id) const;
    std::vector<std::string>
    getEfaDevicesForGpu(int gpu_id) const; // backward compatibility
    int
    detectGpuIdForMemory(void *mem_addr) const;
    bool
    isGpuMemory(void *mem_addr) const;
    // NUMA-based queries
    std::vector<std::string>
    getLibfabricDevicesForNumaNode(int numa_node) const;
    std::vector<std::string>
    getEfaDevicesForNumaNode(int numa_node) const; // backward compatibility
    int
    detectNumaNodeForMemory(void *mem_addr) const;
    bool
    isHostMemory(void *mem_addr) const;

    // Memory-based queries (main interface)
    std::vector<std::string>
    getLibfabricDevicesForMemory(void *mem_addr, nixl_mem_t mem_type) const;
    std::vector<std::string>
    getEfaDevicesForMemory(void *mem_addr, nixl_mem_t mem_type) const; // backward compatibility

    // System information
    int
    getNumGpus() const {
        return num_gpus;
    }

    int
    getNumNumaNodes() const {
        return num_numa_nodes;
    }

    const std::vector<std::string> &
    getAllLibfabricDevices() const {
        return all_libfabric_devices;
    }

    const std::vector<std::string> &
    getAllEfaDevices() const { // backward compatibility
        return all_libfabric_devices;
    }

    // Validation
    bool
    isTopologyDiscovered() const {
        return topology_discovered;
    }

    bool
    isValidGpuId(int gpu_id) const;
    bool
    isValidNumaNode(int numa_node) const;
    bool
    isValidLibfabricDevice(const std::string &device) const;
    bool
    isValidEfaDevice(const std::string &efa_device) const; // backward compatibility
    // Debug/info
    void
    printTopologyInfo() const;
    std::string
    getTopologyString() const;
};

#endif // NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_TOPOLOGY_H
