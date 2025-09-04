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

namespace nixlOfiUtils {

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

} // namespace nixlOfiUtils

#endif