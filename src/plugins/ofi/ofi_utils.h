/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 Intel Corporation. All rights reserved.
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

#ifndef __OFI_UTILS_H
#define __OFI_UTILS_H

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include "habanalabs/synapse_api.h"
#include <cstdint>
#include <string>
#include <nixl_types.h>

namespace nixlOfiUtils {

// HMEM detection utilities
bool determineHmemRequirement();

// memory registration utilities
uint64_t getMemoryRegistrationAccessFlags(const struct fi_info* fi_info);

// device validation
bool validateSynapseAIDevice(uint64_t device_id);
bool validateCudaDevice(uint64_t device_id);
bool validateZeDevice(uint64_t device_id);

// environment helpers
bool isEnvTrue(const char* env_val);

// synapseAI library management
struct SynapseAILibs {
    void* synapseaiHandle;
    void* hlthunkHandle;
    
    struct {
        synStatus (*synInitialize)(void);
        synStatus (*synDestroy)(void);
        synStatus (*synDeviceAcquireByModuleId)(synDeviceId *pDeviceId, const synModuleId moduleId);
        synStatus (*synDeviceGetInfoV2)(const synDeviceId deviceId, synDeviceInfoV2 *pDeviceInfo);
        synStatus (*synStreamCreateGeneric)(synStreamHandle *pStreamHandle, const synDeviceId deviceId, const uint32_t flags);
        int (*hlthunk_device_mapped_memory_export_dmabuf_fd)(int fd, uint64_t addr, uint64_t size, uint64_t offset, uint32_t flags);
    } ops;
    
    SynapseAILibs();
};

bool loadSynapseAILibraries(SynapseAILibs& libs);
void unloadSynapseAILibraries(SynapseAILibs& libs);

// memory alignment utilities
struct AlignedBuffer {
    uint64_t aligned_addr;
    size_t aligned_size;
    uint64_t offset;
};

AlignedBuffer calculateAlignment(uint64_t addr, size_t size, size_t page_size = 4096);

// provider configuration
struct ProviderConfig {
    std::string name;
    enum fi_ep_type ep_type;
    uint64_t caps;
    uint64_t mode;
    uint64_t mr_mode;
    fi_resource_mgmt resource_mgmt;
    struct fi_tx_attr tx_attr;
    struct fi_rx_attr rx_attr;
    uint32_t addr_format;
    enum fi_progress data_progress;
    enum fi_progress control_progress;
};

const ProviderConfig* findProviderConfig(const std::string& provider_name);

// ofi initialization utilities
struct OfiInitConfig {
    std::string providerName;
    bool needHmem;
    struct fi_info* hints;
    struct fi_info* result;
    
    OfiInitConfig(const std::string& provider, bool hmem) 
        : providerName(provider), needHmem(hmem), hints(nullptr), result(nullptr) {}
    ~OfiInitConfig() {
        if (hints) { fi_freeinfo(hints); hints = nullptr; }
        if (result) { fi_freeinfo(result); result = nullptr; }
    }
};

nixl_status_t initializeOFI(OfiInitConfig& config);
nixl_status_t createAndConfigureHints(OfiInitConfig& config);
nixl_status_t performFiGetinfo(OfiInitConfig& config);
void configureHintsForProvider(struct fi_info* hints, const std::string& provider_name);

// post-fi_getinfo utilities
void sanitizeProviderCapabilities(struct fi_info* info, const std::string& provider_name);

// debug macros
#define NIXL_DEBUG_MR_REGISTRATION_RESULT(buf, size, access_flags, mr_mode, key, mr_ptr) \
    do { \
        NIXL_INFO << "=== MR REGISTRATION DEBUG ==="; \
        NIXL_INFO << "buf: 0x" << std::hex << (uint64_t)(buf) << std::dec; \
        NIXL_INFO << "size: " << (size); \
        NIXL_INFO << "access_flags: 0x" << std::hex << (access_flags) << std::dec; \
        NIXL_INFO << "mr_mode: 0x" << std::hex << (mr_mode) << std::dec; \
        NIXL_INFO << "requested_key: " << (key); \
        if (mr_ptr) { \
            NIXL_INFO << "fi_mr_reg SUCCESS: mr=" << (mr_ptr); \
            NIXL_INFO << "fi_mr_key(*mr): " << fi_mr_key((fid_mr*)(mr_ptr)); \
        } else { \
            NIXL_ERROR << "fi_mr_reg FAILED"; \
        } \
        NIXL_INFO << "============================="; \
    } while(0)

// address conversion utilities
std::string ofiSockaddrToString(const void* addr, size_t addrlen);

// misc
bool isTcpFamily(const std::string& prov);      // "tcp", "tcp;ofi_rxm"
bool isRxmLayer(const std::string& prov);       // contains "ofi_rxm"
bool isRxmProvider(const fi_info* info);
bool isConnectionlessProvider(const fi_info* info);
void normalizeHintsForProvider(struct fi_info* info, const std::string& prov);

} // namespace nixlOfiUtils

#endif
