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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// libfabric version requirements
#define NIXL_OFI_VERSION_MAJOR 1
#define NIXL_OFI_VERSION_MINOR 20
#define NIXL_OFI_VERSION FI_VERSION(NIXL_OFI_VERSION_MAJOR, NIXL_OFI_VERSION_MINOR)

namespace nixlOfiUtils {

bool determineHmemRequirement() {
    // check environment variables to see if HMEM is requested
    const char* hmem_vars[] = {"HMEM_SYNAPSEAI", "HMEM_CUDA", "HMEM_ZE", "HMEM"};
    const char* hmem_names[] = {"SynapseAI", "CUDA", "ZE", "auto-detection"};
    
    for (size_t i = 0; i < 4; ++i) {
        const char* env_val = getenv(hmem_vars[i]);
        if (env_val && (strcmp(env_val, "1") == 0 || strcmp(env_val, "true") == 0)) {
            NIXL_DEBUG << "HMEM forced to " << hmem_names[i] << " via " << hmem_vars[i] << " environment variable";
            return true;
        }
    }
    return false;
}

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

#if 0
// provider configuration data
static const ProviderConfig SUPPORTED_PROVIDERS[] = {
    {   
        "shm",
        FI_EP_RDM,
        FI_HMEM, // not implemented Gaudi HBM
        0,  // let provider choose mode
        0,  // let provider choose MR mode  
        FI_RM_UNSPEC,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_UNSPEC}, // tx_attr - use selective completion
        {0, 0, 0, 0, 0, 0, 0, 0}, // rx_attr - use selective completion
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_AUTO,
        FI_PROGRESS_AUTO
    },
    {
        "tcp",
        FI_EP_RDM,
        FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_RECV | FI_SEND | FI_REMOTE_READ | FI_REMOTE_WRITE | FI_MULTI_RECV | FI_LOCAL_COMM | FI_REMOTE_COMM,
        FI_CONTEXT | FI_CONTEXT2,
        FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY,
        FI_RM_ENABLED,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_BULK_DATA}, // tx_attr - use selective completion
        {0, 0, 0, 0, 0, 0, 0, 0}, // rx_attr - use selective completion
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_MANUAL,
        FI_PROGRESS_MANUAL
    },
    {
        // verbs provider configuration matching fabtests mr_mode: 0x74
        "verbs",
        FI_EP_RDM,
        FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_RECV | FI_SEND | FI_REMOTE_READ | FI_REMOTE_WRITE | FI_MULTI_RECV | FI_LOCAL_COMM | FI_REMOTE_COMM,
        FI_CONTEXT,
        FI_MR_LOCAL | FI_MR_RAW | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT, // match fabtests 0x74
        FI_RM_ENABLED,
        {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_BULK_DATA}, // tx_attr - use selective completion like fabtests
        {0, 0, 0, 0, 0, 0, 0, 0}, // rx_attr - use selective completion
        FI_FORMAT_UNSPEC,
        FI_PROGRESS_AUTO,
        FI_PROGRESS_MANUAL
    }
};
#endif

static const ProviderConfig SUPPORTED_PROVIDERS[] = {
  // shm
  { "shm",
    FI_EP_RDM,
    /* caps */ FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE,
    /* mode */ 0,
    /* mr_mode */ 0,                // let provider choose
    FI_RM_UNSPEC,
    /* tx_attr */ {0},              // size=0 => provider default
    /* rx_attr */ {0, 0, 0, 0, 0, 0, 0, 0}, // use selective completion              // size=0 => provider default
    FI_FORMAT_UNSPEC,
    FI_PROGRESS_AUTO,
    FI_PROGRESS_AUTO
  },

  // tcp (and tcp;ofi_rxm)
  { "tcp",
    FI_EP_RDM,                      // RDM, not MSG
    /* caps */ FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_RECV | FI_SEND
              | FI_REMOTE_READ | FI_REMOTE_WRITE | FI_LOCAL_COMM | FI_REMOTE_COMM,
    /* mode */ FI_CONTEXT | FI_CONTEXT2,
    /* mr_mode */ 0,                // provider decides
    FI_RM_ENABLED,
    /* tx_attr */ {0, 0, 0, 0, 0, 0, 0, 0, FI_TC_UNSPEC}, // use selective completion
    /* rx_attr */ {0, 0, 0, 0, 0, 0, 0, 0}, // use selective completion              // size=0 (or ≤ provider limit, e.g. 1024)
    FI_FORMAT_UNSPEC,
    FI_PROGRESS_AUTO,
    FI_PROGRESS_AUTO
  },

  // verbs core (used under verbs;ofi_rxm) - let fi_getinfo determine endpoint type
  { "verbs",
    FI_EP_RDM,                      // RDM, not MSG
    /* caps */ FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_RECV | FI_SEND
              | FI_REMOTE_READ | FI_REMOTE_WRITE | FI_LOCAL_COMM | FI_REMOTE_COMM,
    /* mode */ FI_CONTEXT,
    /* mr_mode */ FI_MR_LOCAL | FI_MR_RAW | FI_MR_VIRT_ADDR
               | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT,
    FI_RM_ENABLED,
    /* tx_attr */ {0, 0, FI_COMPLETION, 0, 0, 0, 0, 0, FI_TC_UNSPEC}, // enable completions
    /* rx_attr */ {0, 0, FI_COMPLETION, 0, 0, 0, 0, 0}, // enable completions
    FI_FORMAT_UNSPEC,
    FI_PROGRESS_AUTO,
    FI_PROGRESS_MANUAL
  },
};

static const size_t NUM_SUPPORTED_PROVIDERS = sizeof(SUPPORTED_PROVIDERS) / sizeof(SUPPORTED_PROVIDERS[0]);

const ProviderConfig* findProviderConfig(const std::string& provider_name) {
    // first try exact match
    for (size_t i = 0; i < NUM_SUPPORTED_PROVIDERS; ++i) {
        if (SUPPORTED_PROVIDERS[i].name == provider_name) {
            return &SUPPORTED_PROVIDERS[i];
        }
    }
    
    // if no exact match and this is an RXM provider, try base provider
    if (isRxmLayer(provider_name)) {
        // extract base provider (everything before first ';')
        size_t pos = provider_name.find(';');
        if (pos != std::string::npos) {
            std::string base_provider = provider_name.substr(0, pos);
            for (size_t i = 0; i < NUM_SUPPORTED_PROVIDERS; ++i) {
                if (SUPPORTED_PROVIDERS[i].name == base_provider) {
                    return &SUPPORTED_PROVIDERS[i];
                }
            }
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
    if (config->tx_attr.op_flags != 0) {
        hints->tx_attr->op_flags = config->tx_attr.op_flags;
    }
    if (config->rx_attr.op_flags != 0) {
        hints->rx_attr->op_flags = config->rx_attr.op_flags;
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

    // always set the provider name in the hints
    if (hints->fabric_attr->prov_name) free(hints->fabric_attr->prov_name);
    hints->fabric_attr->prov_name = strdup(provider_name.c_str());
    
    // apply provider-specific normalization
    normalizeHintsForProvider(hints, provider_name);
}

nixl_status_t createAndConfigureHints(OfiInitConfig& config) {
    config.hints = fi_allocinfo();
    if (!config.hints) {
        NIXL_ERROR << "fi_allocinfo failed";
        return NIXL_ERR_BACKEND;
    }

    // use predefined configuration from SUPPORTED_PROVIDERS
    configureHintsForProvider(config.hints, config.providerName);
    
    // determine HMEM requirements if not explicitly set  
    if (!config.needHmem) {
        config.needHmem = determineHmemRequirement();
    }
    
    // conditionally add HMEM capabilities only when needed
    if (config.needHmem) {
        NIXL_DEBUG << "adding HMEM capabilities for provider: " << config.providerName;
        config.hints->caps |= FI_HMEM;
        config.hints->domain_attr->mr_mode |= FI_MR_HMEM;
    } else {
        NIXL_DEBUG << "skipping HMEM capabilities (not needed) for provider: " << config.providerName;
        // ensure HMEM capabilities are not set
        config.hints->caps &= ~FI_HMEM;
        config.hints->domain_attr->mr_mode &= ~FI_MR_HMEM;
    }

    return NIXL_SUCCESS;
}

// helper function to validate that the selected provider meets all requirements
static void validateProviderCapabilities(const OfiInitConfig& config) {
#define NIXL_CHECK_PROVIDER_ATTR(condition, attr_name, req_val, res_val, to_str_type) \
    NIXL_ASSERT_ALWAYS(condition) \
        << "Provider does not support requested " << attr_name << ". " \
        << "Requested: " << fi_tostr(&(req_val), to_str_type) \
        << ", Got: " << fi_tostr(&(res_val), to_str_type)

    // caps
    NIXL_CHECK_PROVIDER_ATTR((config.result->caps & config.hints->caps) == config.hints->caps,
        "capabilities", config.hints->caps, config.result->caps, FI_TYPE_CAPS);

    // modes
    NIXL_CHECK_PROVIDER_ATTR((config.result->mode & config.hints->mode) == config.hints->mode,
        "modes", config.hints->mode, config.result->mode, FI_TYPE_MODE);

    // EP type
    NIXL_CHECK_PROVIDER_ATTR(config.result->ep_attr->type == config.hints->ep_attr->type,
        "endpoint type", config.hints->ep_attr->type, config.result->ep_attr->type, FI_TYPE_EP_TYPE);

    // mr_mode
    if (config.hints->domain_attr->mr_mode != 0) {
        NIXL_CHECK_PROVIDER_ATTR((config.result->domain_attr->mr_mode & config.hints->domain_attr->mr_mode) == config.hints->domain_attr->mr_mode,
            "mr_mode", config.hints->domain_attr->mr_mode, config.result->domain_attr->mr_mode, FI_TYPE_MR_MODE);
    }

    // threading model
    if (config.hints->domain_attr->threading != FI_THREAD_UNSPEC) {
        NIXL_CHECK_PROVIDER_ATTR(config.result->domain_attr->threading == config.hints->domain_attr->threading,
            "threading model", config.hints->domain_attr->threading, config.result->domain_attr->threading, FI_TYPE_THREADING);
    }

    // resource management model
    if (config.hints->domain_attr->resource_mgmt != FI_RM_UNSPEC) {
        NIXL_ASSERT_ALWAYS(config.result->domain_attr->resource_mgmt == config.hints->domain_attr->resource_mgmt)
            << "Provider does not support requested resource_mgmt. "
            << "Requested: " << (config.hints->domain_attr->resource_mgmt == FI_RM_ENABLED ? "FI_RM_ENABLED" : "OTHER")
            << ", Got: " << (config.result->domain_attr->resource_mgmt == FI_RM_ENABLED ? "FI_RM_ENABLED" : "OTHER");
    }

    // address format
    if (config.hints->addr_format != FI_FORMAT_UNSPEC) {
        NIXL_CHECK_PROVIDER_ATTR(config.result->addr_format == config.hints->addr_format,
            "address format", config.hints->addr_format, config.result->addr_format, FI_TYPE_ADDR_FORMAT);
    }

    // tx op_flags
    if (config.hints->tx_attr->op_flags != 0) {
        NIXL_CHECK_PROVIDER_ATTR((config.result->tx_attr->op_flags & config.hints->tx_attr->op_flags) == config.hints->tx_attr->op_flags,
            "tx_attr->op_flags", config.hints->tx_attr->op_flags, config.result->tx_attr->op_flags, FI_TYPE_OP_FLAGS);
    }

    // rx op_flags
    if (config.hints->rx_attr->op_flags != 0) {
        NIXL_CHECK_PROVIDER_ATTR((config.result->rx_attr->op_flags & config.hints->rx_attr->op_flags) == config.hints->rx_attr->op_flags,
            "rx_attr->op_flags", config.hints->rx_attr->op_flags, config.result->rx_attr->op_flags, FI_TYPE_OP_FLAGS);
    }

#undef NIXL_CHECK_PROVIDER_ATTR
}

nixl_status_t performFiGetinfo(OfiInitConfig& config) {
    printf("=== NIXL OFI DEBUG: Before fi_getinfo ===\n");
    printf("FI_VERSION: 1.20\n");
    printf("hints->caps: %s\n", fi_tostr(&config.hints->caps, FI_TYPE_CAPS));
    printf("hints->mode: %s\n", fi_tostr(&config.hints->mode, FI_TYPE_MODE));
    printf("hints->ep_attr->type: %s\n", fi_tostr(&config.hints->ep_attr->type, FI_TYPE_EP_TYPE));
    printf("hints->domain_attr->mr_mode: %s\n", fi_tostr(&config.hints->domain_attr->mr_mode, FI_TYPE_MR_MODE));
    printf("hints->domain_attr->threading: %s\n", fi_tostr(&config.hints->domain_attr->threading, FI_TYPE_THREADING));
    printf("hints->domain_attr->resource_mgmt: %s\n", config.hints->domain_attr->resource_mgmt == FI_RM_ENABLED ? "FI_RM_ENABLED" : (config.hints->domain_attr->resource_mgmt == FI_RM_UNSPEC ? "FI_RM_UNSPEC" : "OTHER"));
    printf("hints->addr_format: %s\n", fi_tostr(&config.hints->addr_format, FI_TYPE_ADDR_FORMAT));
    printf("provider name: %s\n", config.hints->fabric_attr->prov_name ? config.hints->fabric_attr->prov_name : "NULL");
    printf("==========================================\n");
    fflush(stdout);

    int ret = fi_getinfo(NIXL_OFI_VERSION, nullptr, nullptr, 0, config.hints, &config.result);
    if (ret) {
        NIXL_ERROR << "fi_getinfo failed: " << fi_strerror(-ret);
        printf("=== NIXL OFI DEBUG: fi_getinfo FAILED ===\n");
        printf("Error: %s\n", fi_strerror(-ret));
        printf("==========================================\n");
        fflush(stdout);
        return NIXL_ERR_BACKEND;
    }

    printf("=== NIXL OFI DEBUG: After fi_getinfo SUCCESS ===\n");
    if (config.result) {
        printf("selected provider: %s\n", config.result->fabric_attr->prov_name ? config.result->fabric_attr->prov_name : "unknown");
        printf("selected caps: %s\n", fi_tostr(&config.result->caps, FI_TYPE_CAPS));
        printf("selected mode: %s\n", fi_tostr(&config.result->mode, FI_TYPE_MODE));
        printf("selected ep_type: %s\n", fi_tostr(&config.result->ep_attr->type, FI_TYPE_EP_TYPE));
        printf("selected mr_mode: %s\n", fi_tostr(&config.result->domain_attr->mr_mode, FI_TYPE_MR_MODE));
        printf("selected threading: %s\n", fi_tostr(&config.result->domain_attr->threading, FI_TYPE_THREADING));
    }
    printf("==============================================\n");
    fflush(stdout);

    if (!config.result) {
        NIXL_ERROR << "fi_getinfo returned null result";
        return NIXL_ERR_BACKEND;
    }

    validateProviderCapabilities(config);

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
    status = performFiGetinfo(config);
    if (status != NIXL_SUCCESS) {
        return status;
    }
    
    // step 3: sanitize capabilities based on actual provider support
    sanitizeProviderCapabilities(config.result, config.providerName);
    
    return NIXL_SUCCESS;
}

// misc
// connectionless detection - single source of truth
bool isRxmProvider(const fi_info* info) {
    if (!info || !info->fabric_attr || !info->fabric_attr->prov_name) return false;
    std::string p(info->fabric_attr->prov_name);
    return p.find("ofi_rxm") != std::string::npos;
}

bool isConnectionlessProvider(const fi_info* info) {
    if (!info || !info->ep_attr) return false;
    switch (info->ep_attr->type) {
        case FI_EP_DGRAM: return true;   // always connectionless
        case FI_EP_RDM:   return true;   // RDM is connectionless (including RXM)
        default:          return false;  // MSG and others are connection-oriented
    }
}

static inline bool contains_ci(const std::string& k, const char* n) {
    auto pos = std::search(
        k.begin(), k.end(),
        n, n + std::strlen(n),
        [](char a, char b){ return std::tolower(a) == std::tolower(b); });
    return pos != k.end();
}

bool isTcpFamily(const std::string& prov) {
    // matches "tcp" and layered "tcp;ofi_rxm"
    return contains_ci(prov, "tcp");
}

bool isRxmLayer(const std::string& prov) {
    return contains_ci(prov, "ofi_rxm");
}

// strip caps that a provider family cannot satisfy.
static inline uint64_t nixlStripUnsupportedCaps(uint64_t caps, const std::string& prov) {
    // TCP does not support FI_MULTI_RECV
    if (isTcpFamily(prov)) {
        caps &= ~FI_MULTI_RECV;
    }
    return caps;
}

// normalize endpoint type, addr_format, tx/rx sizes, progress, etc.
// called *after* info from ProviderConfig is filled in but *before* fi_getinfo()
void normalizeHintsForProvider(struct fi_info* info, const std::string& prov)
{
    if (!info) return;

    // ensure sub-structs exist if caller didn’t allocate them.
    if (!info->ep_attr)  info->ep_attr  = (fi_ep_attr*) calloc(1, sizeof(*info->ep_attr));
    if (!info->tx_attr)  info->tx_attr  = (fi_tx_attr*) calloc(1, sizeof(*info->tx_attr));
    if (!info->rx_attr)  info->rx_attr  = (fi_rx_attr*) calloc(1, sizeof(*info->rx_attr));
    if (!info->fabric_attr) info->fabric_attr = (fi_fabric_attr*) calloc(1, sizeof(*info->fabric_attr));

    // tcp family still needs RDM, but let ofi_rxm provider determine its own endpoint type
    if (isTcpFamily(prov) && !isRxmLayer(prov)) {
        info->ep_attr->type = FI_EP_RDM;
    }
    // for RXM providers (like verbs;ofi_rxm), let fi_getinfo determine the correct endpoint type

    // address format
    if (isTcpFamily(prov)) {
        info->addr_format = (info->addr_format == FI_FORMAT_UNSPEC) ? FI_SOCKADDR_IN : info->addr_format;
    }

    // capabilities cleanup
    info->caps = nixlStripUnsupportedCaps(info->caps, prov);

    // RX/TX queue sizes - always let provider pick optimal sizes
    size_t tx_sz = 0;  // let provider pick
    size_t rx_sz = 0;  // let provider pick

    info->tx_attr->size = tx_sz;  // 0 = let provider choose
    info->rx_attr->size = rx_sz;  // 0 = let provider choose
    
    NIXL_INFO << "normalizeHintsForProvider: set tx_attr.size=" << info->tx_attr->size << " rx_attr.size=" << info->rx_attr->size << " (0=provider chooses)";
    NIXL_INFO << "normalizeHintsForProvider: tx_attr.op_flags=0x" << std::hex << info->tx_attr->op_flags << std::dec << " rx_attr.op_flags=0x" << std::hex << info->rx_attr->op_flags << std::dec;
    NIXL_INFO << "normalizeHintsForProvider: FI_COMPLETION=0x" << std::hex << FI_COMPLETION << std::dec << " (should be in both tx and rx op_flags)";

    // progress model; RXM benefits from provider-driven progress
    if (isRxmLayer(prov) || isTcpFamily(prov)) {
        if (info->domain_attr) {
            if (info->domain_attr->data_progress == FI_PROGRESS_UNSPEC)
                info->domain_attr->data_progress = FI_PROGRESS_AUTO;
            if (info->domain_attr->control_progress == FI_PROGRESS_UNSPEC)
                info->domain_attr->control_progress = FI_PROGRESS_AUTO;
        }
    }

    // MR mode. TCP and RXM: don't force strong MR modes; leave provider choosen
    if (info->domain_attr && isTcpFamily(prov)) {
        if (info->domain_attr->mr_mode != 0) {
            // tcp provider supports scalable MR; letting it pick avoids "unsupported" debug noise.
            info->domain_attr->mr_mode = 0;
        }
    }

    // provider name recorded in hints
    if (!info->fabric_attr->prov_name) {
        info->fabric_attr->prov_name = strdup(prov.c_str());
    }
}

void sanitizeProviderCapabilities(struct fi_info* info, const std::string& provider_name) {
    if (!info) return;
    
    // remove capabilities that the provider doesn't actually support
    info->caps = nixlStripUnsupportedCaps(info->caps, provider_name);
    
    // ensure essential RMA capabilities are present if provider supports them
    if (info->caps & (FI_RMA | FI_READ | FI_WRITE)) {
        // provider supports RMA, ensure remote capabilities are also set
        info->caps |= FI_REMOTE_READ | FI_REMOTE_WRITE;
    }
}

std::string ofiSockaddrToString(const void* addr, size_t addrlen) {
    if (!addr || addrlen == 0) {
        return "null-address";
    }
    
    const struct sockaddr* sa = static_cast<const struct sockaddr*>(addr);
    char buf[INET6_ADDRSTRLEN + 16]; // extra space for port
    
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            return std::string(buf) + ":" + std::to_string(ntohs(sin->sin_port));
        }
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
            return "[" + std::string(buf) + "]:" + std::to_string(ntohs(sin6->sin6_port));
        }
    }
    
    // fallback for unknown address families
    char hex_buf[256];
    const uint8_t* bytes = static_cast<const uint8_t*>(addr);
    std::string result = "unknown-family-" + std::to_string(sa->sa_family) + "-";
    for (size_t i = 0; i < std::min(addrlen, size_t(32)); i++) {
        snprintf(hex_buf + i*2, 3, "%02x", bytes[i]);
    }
    return result + hex_buf;
}


} // namespace nixlOfiUtils
