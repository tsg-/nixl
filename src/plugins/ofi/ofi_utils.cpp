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
#include <rdma/fi_errno.h>

// libfabric version requirements
#define NIXL_OFI_VERSION_MAJOR 1
#define NIXL_OFI_VERSION_MINOR 20
#define NIXL_OFI_VERSION FI_VERSION(NIXL_OFI_VERSION_MAJOR, NIXL_OFI_VERSION_MINOR)

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

// provider configuration data
static const ProviderConfig SUPPORTED_PROVIDERS[] = {
    {   
        "shm",
        FI_EP_RDM,
        FI_HMEM, // not implemented Gaudi HBM
        0,  // let provider choose mode
        0,  // let provider choose MR mode  
        FI_RM_UNSPEC,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_UNSPEC}, // tx_attr defaults
        {0, 0, 0, 0, 0, 0}, // rx_attr defaults
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_AUTO,
        FI_PROGRESS_AUTO
    },
    {
        "tcp",
        FI_EP_MSG,
        FI_MSG | FI_RMA | FI_READ | FI_WRITE,
        FI_CONTEXT | FI_CONTEXT2,
        0, // let provider choose mr_mode
        FI_RM_ENABLED,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_BULK_DATA}, // tx_attr with bulk data class
        {0, 0, 0, 0, 0, 0}, // rx_attr defaults
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_MANUAL,
        FI_PROGRESS_MANUAL
    },
    {
        // Match verbs;ofi_rxm capabilities from fi_info output
        "verbs",
        FI_EP_RDM,
        FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_RECV | FI_SEND | FI_REMOTE_READ | FI_REMOTE_WRITE | FI_MULTI_RECV | FI_LOCAL_COMM | FI_REMOTE_COMM | FI_HMEM,
        0,
        FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_HMEM,
        FI_RM_ENABLED,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_BULK_DATA}, // tx_attr with bulk data class like fabtests
        {0, 0, 0, 0, 0, 0}, // rx_attr defaults
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_AUTO,
        FI_PROGRESS_MANUAL
    }
};

static const size_t NUM_SUPPORTED_PROVIDERS = sizeof(SUPPORTED_PROVIDERS) / sizeof(SUPPORTED_PROVIDERS[0]);

const ProviderConfig* findProviderConfig(const std::string& provider_name) {
    for (size_t i = 0; i < NUM_SUPPORTED_PROVIDERS; ++i) {
        if (SUPPORTED_PROVIDERS[i].name == provider_name) {
            return &SUPPORTED_PROVIDERS[i];
        }
    }
    return nullptr;
}

void configureHintsForProvider(struct fi_info* hints, const std::string& provider_name) {
    const auto* config = findProviderConfig(provider_name);

    if (!config) {
        // if the provider is not in our list, use the verbs config as a safe default
        config = findProviderConfig("verbs");
        NIXL_DEBUG << "unknown provider '" << provider_name << "', using verbs config as fallback";
    } else {
        NIXL_DEBUG << "using predefined config for provider: " << provider_name;
    }

    // apply the configuration from the data structure
    hints->ep_attr->type = config->ep_type;
    hints->domain_attr->resource_mgmt = config->resource_mgmt;
    hints->caps = config->caps;
    hints->mode = config->mode;

    if (config->mr_mode != 0) {
        hints->domain_attr->mr_mode = config->mr_mode;
    }

    // apply tx/rx attributes
    if (config->tx_attr.tclass != 0) {
        hints->tx_attr->tclass = config->tx_attr.tclass;
    }
    
    // address format - only set if not UNSPEC
    if (config->addr_format != FI_FORMAT_UNSPEC) {
        hints->addr_format = config->addr_format;
    }

    // progress models - only set if not UNSPEC 
    if (config->data_progress != FI_PROGRESS_UNSPEC) {
        hints->domain_attr->data_progress = config->data_progress;
    }
    if (config->control_progress != FI_PROGRESS_UNSPEC) {
        hints->domain_attr->control_progress = config->control_progress;
    }

    // enable shared RX context for verbs providers to use XRC endpoints like fabtests
    // but only for connection-oriented (FI_EP_MSG) endpoints, not RDM
    if (provider_name.find("verbs") != std::string::npos && config->ep_type == FI_EP_MSG) {
        hints->ep_attr->rx_ctx_cnt = FI_SHARED_CONTEXT;
    }

    // always set the provider name in the hints
    if (hints->fabric_attr->prov_name) free(hints->fabric_attr->prov_name);
    hints->fabric_attr->prov_name = strdup(provider_name.c_str());
}

nixl_status_t createAndConfigureHints(OfiInitConfig& config) {
    config.hints = fi_allocinfo();
    if (!config.hints) {
        NIXL_ERROR << "fi_allocinfo failed";
        return NIXL_ERR_BACKEND;
    }

    // for tcp;ofi_rxm, verbs;ofi_rxm: use minimal config path
    if (config.providerName == "tcp;ofi_rxm" || config.providerName == "verbs;ofi_rxm") {
        NIXL_DEBUG << "using minimal config for rxm provider: " << config.providerName;
        
        // minimal configuration for RXM providers
        config.hints->fabric_attr->prov_name = strdup(config.providerName.c_str());
    } else {
        // for other providers, use predefined configuration
        configureHintsForProvider(config.hints, config.providerName);
    }

    // force rma capabilities
    config.hints->caps |= FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    NIXL_INFO << "adding rma capabilities to hints: FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE";

    // debug print all configured hints
    NIXL_INFO << "=== constructor: fi_getinfo hints ===";
    NIXL_INFO << "provider name: " << config.providerName;
    NIXL_INFO << "caps: " << fi_tostr(&config.hints->caps, FI_TYPE_CAPS);
    NIXL_INFO << "mode: " << fi_tostr(&config.hints->mode, FI_TYPE_MODE);
    NIXL_INFO << "ep_attr->type: " << fi_tostr(&config.hints->ep_attr->type, FI_TYPE_EP_TYPE);
    NIXL_INFO << "domain_attr->mr_mode: " << fi_tostr(&config.hints->domain_attr->mr_mode, FI_TYPE_MR_MODE);
    NIXL_INFO << "domain_attr->resource_mgmt: " << config.hints->domain_attr->resource_mgmt;
    NIXL_INFO << "addr_format: " << fi_tostr(&config.hints->addr_format, FI_TYPE_ADDR_FORMAT);
    NIXL_INFO << "========================";

    return NIXL_SUCCESS;
}

nixl_status_t performFiGetinfo(OfiInitConfig& config) {
    int ret = fi_getinfo(NIXL_OFI_VERSION, nullptr, nullptr, 0, config.hints, &config.result);
    if (ret) {
        NIXL_ERROR << "fi_getinfo failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    if (!config.result) {
        NIXL_ERROR << "fi_getinfo returned null result";
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}

nixl_status_t initializeOFI(OfiInitConfig& config) {
    NIXL_INFO << "hmem support requested: " << (config.needHmem ? "YES" : "NO");
    
    // step 1: create and configure hints with all required capabilities
    nixl_status_t status = createAndConfigureHints(config);
    if (status != NIXL_SUCCESS) {
        return status;
    }
    
    // step 2: perform fi_getinfo with fallback handling
    return performFiGetinfo(config);
}

} // namespace nixlOfiUtils