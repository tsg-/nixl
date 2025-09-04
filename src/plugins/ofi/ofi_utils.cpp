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

#include "ofi_utils.h"
#include "common/nixl_log.h"
#include <cstring>
#include <string>
#include <unistd.h>
#include <dlfcn.h>

namespace nixlOfiUtils {

uint64_t getMemoryRegistrationAccessFlags(const struct fi_info* fi_info) {
    uint64_t access_flags = FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV;
    
    if (fi_info && fi_info->domain_attr) {
        if (fi_info->caps & FI_READ) access_flags |= FI_READ;
        if (fi_info->caps & FI_WRITE) access_flags |= FI_WRITE;
        if (fi_info->caps & FI_RMA) {
            access_flags |= FI_READ | FI_WRITE;
        }
    }
    
    return access_flags;
}

bool validateSynapseAIDevice(uint64_t device_id) {
    std::string device_path = "/dev/accel/accel" + std::to_string(device_id);
    if (access(device_path.c_str(), R_OK | W_OK) != 0) {
        NIXL_INFO << "synapseAI device " << device_path << " not accessible, will fallback to system memory";
        return false;
    }
    return true;
}

bool validateCudaDevice(uint64_t device_id) {
    // TODO: add proper cuda device validation
    return true;
}

bool validateZeDevice(uint64_t device_id) {
    // TODO: add proper ze device validation
    return true;
}

bool isEnvTrue(const char* env_val) {
    return env_val && (strcmp(env_val, "1") == 0 || strcmp(env_val, "true") == 0);
}

SynapseAILibs::SynapseAILibs() : synapseaiHandle(nullptr), hlthunkHandle(nullptr) {
    memset(&ops, 0, sizeof(ops));
}

bool loadSynapseAILibraries(SynapseAILibs& libs) {
    // load synapse library
    if (!libs.synapseaiHandle) {
        libs.synapseaiHandle = dlopen("libSynapse.so", RTLD_NOW);
        if (!libs.synapseaiHandle) {
            NIXL_ERROR << "failed to dlopen libSynapse.so: " << dlerror();
            return false;
        }
        
        libs.ops.synDeviceGetInfoV2 = 
            (synStatus (*)(const synDeviceId, synDeviceInfoV2 *))dlsym(libs.synapseaiHandle, "synDeviceGetInfoV2");
        if (!libs.ops.synDeviceGetInfoV2) {
            NIXL_ERROR << "failed to find synDeviceGetInfoV2: " << dlerror();
            return false;
        }
    }
    
    // load hlthunk library
    if (!libs.hlthunkHandle) {
        libs.hlthunkHandle = dlopen("libhl-thunk.so", RTLD_NOW);
        if (!libs.hlthunkHandle) {
            NIXL_ERROR << "failed to dlopen libhl-thunk.so: " << dlerror();
            return false;
        }
        
        libs.ops.hlthunk_device_mapped_memory_export_dmabuf_fd = 
            (int (*)(int, uint64_t, uint64_t, uint64_t, uint32_t))dlsym(libs.hlthunkHandle, "hlthunk_device_mapped_memory_export_dmabuf_fd");
        if (!libs.ops.hlthunk_device_mapped_memory_export_dmabuf_fd) {
            NIXL_ERROR << "failed to find hlthunk_device_mapped_memory_export_dmabuf_fd: " << dlerror();
            return false;
        }
    }
    
    return true;
}

void unloadSynapseAILibraries(SynapseAILibs& libs) {
    if (libs.synapseaiHandle) {
        dlclose(libs.synapseaiHandle);
        libs.synapseaiHandle = nullptr;
    }
    if (libs.hlthunkHandle) {
        dlclose(libs.hlthunkHandle);
        libs.hlthunkHandle = nullptr;
    }
    memset(&libs.ops, 0, sizeof(libs.ops));
}

AlignedBuffer calculateAlignment(uint64_t addr, size_t size, size_t page_size) {
    AlignedBuffer result;
    
    // align address down to page boundary
    result.aligned_addr = (addr / page_size) * page_size;
    result.offset = addr - result.aligned_addr;
    
    // align size up to include full pages
    result.aligned_size = (size + result.offset + page_size - 1) & ~(page_size - 1);
    
    return result;
}

} // namespace nixlOfiUtils