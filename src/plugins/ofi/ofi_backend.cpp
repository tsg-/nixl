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

#include "ofi_backend.h"
#include "common/nixl_log.h"
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_endpoint.h>
#include <stdexcept>
#include <unistd.h>
#include <functional>
#include <fcntl.h>

#include <cstdlib>

// static synapseAI handles for dynamic loading
void* nixlOfiEngine::synapseai_handle_ = nullptr;
void* nixlOfiEngine::hlthunk_handle_ = nullptr;
nixlOfiEngine::synapseai_ops nixlOfiEngine::synapseai_ops_ = {};
static std::mutex synapseai_init_mutex_;

nixlOfiEngine::nixlOfiEngine(const nixlBackendInitParams* init_params) :
    nixlBackendEngine(init_params),
    fabric_(nullptr),
    domain_(nullptr),
    ep_(nullptr),
    cq_(nullptr),
    eq_(nullptr),
    pep_(nullptr),
    fi_(nullptr),
    cachedProviderInfo_(nullptr),
    av_(nullptr),
    isConnectionless_(false),
    eqThreadStop_(false),
    eqThreadPaused_(false),
    eqTimeoutMs_(100),
    hmemZeSupported_(false),
    hmemCudaSupported_(false),
    hmemSynapseaiSupported_(false)
{
    localAgentName_ = init_params->localAgent;
    struct fi_info *hints = nullptr;
    struct fi_info *info = nullptr;
    int ret = 0;

    // use FI_PROVIDER environment variable or fall back to sensible defaults
    const char* env_provider = getenv("FI_PROVIDER");
    if (env_provider) {
        providerName_ = env_provider;
        NIXL_INFO << "Using FI_PROVIDER=" << providerName_;
        
        // early guess for connection model (corrected after fi_getinfo)
        isConnectionless_ = decideConnectionlessFromName(providerName_);
        NIXL_DEBUG << "pre-fi_getinfo guess: connectionless=" << isConnectionless_;
    } else {
        // Default to verbs provider
        providerName_ = "verbs";
        NIXL_DEBUG << "Using default provider: " << providerName_;
        isConnectionless_ = false; // conservative default
    }

    // validate that the provider is supported (skip validation for minimal config providers)
    bool useMinimalConfig = (providerName_ == "shm" || providerName_ == "tcp" || 
                             providerName_ == "tcp;ofi_rxm" || providerName_ == "verbs;ofi_rxm");
    if (!useMinimalConfig) {
        const auto* config = findProviderConfig(providerName_);
        if (!config) {
            NIXL_ERROR << "Unsupported provider: " << providerName_;
            NIXL_ERROR << "Supported providers: shm, tcp, tcp;ofi_rxm, verbs, verbs;ofi_rxm";
            this->initErr = true;
            return;
        }
    }

    // get EQ timeout parameter (0-60 seconds max)
    getLongParam(init_params, "eq_timeout_ms", eqTimeoutMs_, 0, 60000);

    hints = fi_allocinfo();
    if (!hints) {
        this->initErr = true;
        NIXL_ERROR << "fi_allocinfo failed";
        return;
    }

    // HMEM determination strategy:
    // 1 environment variables: HMEM=1 (auto-detect), HMEM_SYNAPSEAI=1, HMEM_CUDA=1, HMEM_ZE=1
    // 2 application registers VRAM memory type during registerMem() call
    // 3 device discovery during registration if not overridden
    
    bool need_hmem = false;
    
    // check HMEM environment variables
    auto isEnvTrue = [](const char* env_val) -> bool {
        return env_val && (strcmp(env_val, "1") == 0 || strcmp(env_val, "true") == 0);
    };
    
    const char* hmem_vars[] = {"HMEM_SYNAPSEAI", "HMEM_CUDA", "HMEM_ZE", "HMEM"};
    const char* hmem_names[] = {"SynapseAI", "CUDA", "ZE", "auto-detection"};
    
    for (size_t i = 0; i < 4; ++i) {
        const char* env_val = getenv(hmem_vars[i]);
        if (isEnvTrue(env_val)) {
            need_hmem = true;
            NIXL_DEBUG << "HMEM forced to " << hmem_names[i] << " via " << hmem_vars[i] << " environment variable";
            break;
        }
    }

    NIXL_INFO << "HMEM support requested: " << (need_hmem ? "YES" : "NO");

    if (need_hmem) {
        hints->caps |= FI_HMEM;
        NIXL_DEBUG << "Adding FI_HMEM to hints->caps for device memory support";
    } else {
        NIXL_DEBUG << "HMEM not enabled - DRAM memory only";
    }

    // for shm, tcp, and rxm providers, use minimal configuration (only provider name)
    if (providerName_ == "shm" || providerName_ == "tcp" || 
        providerName_ == "tcp;ofi_rxm" || providerName_ == "verbs;ofi_rxm") {
        hints->fabric_attr->prov_name = strdup(providerName_.c_str());
        NIXL_DEBUG << "Using minimal auto-negotiated hints for " << providerName_ << " provider";
    } else {
        // for other providers, use predefined configuration from SUPPORTED_PROVIDERS  
        configureHintsForProvider(hints, providerName_);
    }

    // debug print all hints
    NIXL_INFO << "=== constructor: fi_getinfo hints ===";
    NIXL_INFO << "provider name: " << hints->fabric_attr->prov_name;
    NIXL_INFO << "caps: " << fi_tostr(&hints->caps, FI_TYPE_CAPS);
    NIXL_INFO << "mode: " << fi_tostr(&hints->mode, FI_TYPE_MODE);
    NIXL_INFO << "ep_attr->type: " << fi_tostr(&hints->ep_attr->type, FI_TYPE_EP_TYPE);
    NIXL_INFO << "domain_attr->mr_mode: " << fi_tostr(&hints->domain_attr->mr_mode, FI_TYPE_MR_MODE);
    NIXL_INFO << "domain_attr->resource_mgmt: " << hints->domain_attr->resource_mgmt;
    NIXL_INFO << "addr_format: " << fi_tostr(&hints->addr_format, FI_TYPE_ADDR_FORMAT);
    NIXL_INFO << "========================";

    // let libfabric choose optimal settings; only override if explicitly needed

    ret = fi_getinfo(FI_VERSION(1, 20), nullptr, nullptr, 0, hints, &info);
    if (ret) {
        NIXL_ERROR << "fi_getinfo failed: " << fi_strerror(-ret);
        NIXL_DEBUG << "Trying fi_getinfo with minimal hints for provider " << providerName_;
        
        // minimal hints, see what provider supports
        struct fi_info *minimal_hints = fi_allocinfo();
        if (minimal_hints) {
            minimal_hints->fabric_attr->prov_name = strdup(providerName_.c_str());
            struct fi_info *minimal_fi = nullptr;
            int minimal_ret = fi_getinfo(FI_VERSION(1, 18), nullptr, nullptr, 0, minimal_hints, &minimal_fi);
            if (minimal_ret == 0) {
                NIXL_DEBUG << "Provider " << providerName_ << " supports: caps=0x" << std::hex << minimal_fi->caps 
                         << " ep_type=" << minimal_fi->ep_attr->type;
                fi_freeinfo(minimal_fi);
            } else {
                NIXL_ERROR << "Even minimal fi_getinfo failed: " << fi_strerror(-minimal_ret);
            }
            fi_freeinfo(minimal_hints);
        }
        goto cleanup_teardown;
    }

    // use the first provider returned by fi_getinfo (highest performance)
    fi_ = info;
    if (!fi_) {
        NIXL_ERROR << "No providers returned by fi_getinfo";
        goto cleanup_teardown;
    }
    
    NIXL_DEBUG << "fi_ assigned successfully, checking provider info...";
    NIXL_DEBUG << "Selected provider: " << (fi_->fabric_attr->prov_name ? fi_->fabric_attr->prov_name : "unknown")
               << " with endpoint type: " << fi_tostr(&fi_->ep_attr->type, FI_TYPE_EP_TYPE);
    NIXL_INFO << "Provider capabilities: " << fi_tostr(&fi_->caps, FI_TYPE_CAPS);
    NIXL_INFO << "Provider mode: " << fi_tostr(&fi_->mode, FI_TYPE_MODE);

    // final decision after fi_getinfo(): correct for rxm and endpoint type
    {
        const char* selected_prov = fi_->fabric_attr ? fi_->fabric_attr->prov_name : "<unknown>";
        bool finalConnless = decideConnectionlessFromInfo(fi_);
        if (finalConnless != isConnectionless_) {
            NIXL_DEBUG << "Connection model corrected after fi_getinfo(): provider="
                       << selected_prov << " ep_type=" << (fi_->ep_attr ? fi_->ep_attr->type : -1)
                       << " was=" << (isConnectionless_ ? "connectionless" : "connected")
                       << " now=" << (finalConnless ? "connectionless" : "connected");
            isConnectionless_ = finalConnless;
        }
        
        NIXL_DEBUG << "Provider " << selected_prov << " ep_type=" 
                   << fi_tostr(&fi_->ep_attr->type, FI_TYPE_EP_TYPE) 
                   << " isConnectionless=" << isConnectionless_;
    }

    // detect HMEM capabilities for this provider
    detectHmemCapabilities(fi_, providerName_, hmemCudaSupported_,
                           hmemZeSupported_, hmemSynapseaiSupported_);

    ret = fi_fabric(fi_->fabric_attr, &fabric_, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_fabric failed: " << fi_strerror(-ret);
        goto cleanup_teardown;
    }

    ret = fi_domain(fabric_, fi_, &domain_, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_domain failed: " << fi_strerror(-ret);
        goto cleanup_teardown;
    }

    {
        // RDM endpoints (including RXM) don't use passive endpoints, only MSG endpoints do
        bool needPassiveEndpoint = (fi_->ep_attr->type == FI_EP_MSG);
        nixl_status_t setup_status = setupEndpoint(needPassiveEndpoint);
        if (setup_status != NIXL_SUCCESS) {
            NIXL_ERROR << "setupEndpoint failed with status: " << setup_status;
            goto cleanup_teardown;
        }
        NIXL_DEBUG << "setupEndpoint completed successfully, isConnectionless_=" << isConnectionless_;

        // get local address
        nixl_status_t addr_status = getEndpointAddress(ep_, localAddr_);
        if (addr_status != NIXL_SUCCESS) {
            NIXL_ERROR << "getEndpointAddress() failed with status: " << addr_status;
            goto cleanup_teardown;
        }
        // Debug: print local address blob size and hex dump to help diagnose provider address format
        NIXL_DEBUG << "getEndpointAddress completed successfully, isConnectionless_=" << isConnectionless_;
        {
            std::string dump;
            dump.reserve(localAddr_.size() * 3);
            for (unsigned char c : localAddr_) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x", c);
                dump.append(buf);
                dump.push_back(' ');
            }
            NIXL_DEBUG << "Local endpoint address len=" << localAddr_.size() << " hex=" << dump;
        }
    }

    // cache provider use in connect()
    cachedProviderInfo_ = fi_dupinfo(fi_);
    if (!cachedProviderInfo_) {
        NIXL_WARN << "Failed to duplicate provider info for caching";
    }

    fi_freeinfo(hints);
    // Don't free info here since fi_ = info (line 161), it will be freed in destructor

    NIXL_DEBUG << "Starting EQ event loop ...: connectionless=" << isConnectionless_;
    // start event loop thread for connection-oriented providers
    if (!isConnectionless_) {
        NIXL_DEBUG << "Creating EQ event loop thread for connection-oriented provider";
        eqThread_ = std::thread(&nixlOfiEngine::eq_event_loop, this);
    } else {
        NIXL_DEBUG << "Skipping EQ event loop thread for connectionless provider";
    }
    NIXL_DEBUG << "OFI backend constructor completed successfully";
    return;

cleanup_teardown:
    if (ep_)     { fi_close(&ep_->fid);     ep_ = nullptr; }
    if (av_)     { fi_close(&av_->fid);     av_ = nullptr; }
    if (pep_)    { fi_close(&pep_->fid);    pep_ = nullptr; }
    if (eq_)     { fi_close(&eq_->fid);     eq_ = nullptr; }
    if (cq_)     { fi_close(&cq_->fid);     cq_ = nullptr; }
    if (domain_) { fi_close(&domain_->fid); domain_ = nullptr; }
    if (fabric_) { fi_close(&fabric_->fid); fabric_ = nullptr; }
    if (fi_)     { fi_freeinfo(fi_);        fi_ = nullptr; info = nullptr; }
    if (hints)   { fi_freeinfo(hints);      hints = nullptr; }
    this->initErr = true;
}

void nixlOfiEngine::getStringParam(const nixlBackendInitParams* init_params, const std::string& key, std::string& value) {
    auto it = init_params->customParams->find(key);
    if (it != init_params->customParams->end()) {
        value = it->second;
    }
}

void nixlOfiEngine::getLongParam(const nixlBackendInitParams* init_params, const std::string& key, long& value, long min_val, long max_val) {
    auto it = init_params->customParams->find(key);
    if (it != init_params->customParams->end()) {
        try {
            long parsed_val = std::stol(it->second);
            if (parsed_val >= min_val && parsed_val <= max_val) {
                value = parsed_val;
            } else {
                NIXL_WARN << key << " out of range [" << min_val << "-" << max_val << "]: " << parsed_val << ", using default " << value;
            }
        } catch (const std::exception& e) {
            NIXL_WARN << "Invalid " << key << " parameter: " << it->second << ", using default " << value;
        }
    }
}

void nixlOfiEngine::getSizeTParam(const nixlBackendInitParams* init_params, const std::string& key, size_t& value) {
    auto it = init_params->customParams->find(key);
    if (it != init_params->customParams->end()) {
        try {
            size_t parsed_val = std::stoull(it->second);
            value = parsed_val;
            NIXL_DEBUG << "Set " << key << " to " << value;
        } catch (const std::exception& e) {
            NIXL_WARN << "Invalid " << key << ": " << it->second << ", keeping default " << value;
        }
    }
}

// predefined provider configurations for providers that need explicit settings
// Note: SHM and TCP providers use minimal auto-negotiated configuration instead
const nixlOfiEngine::ProviderConfig nixlOfiEngine::SUPPORTED_PROVIDERS[] = {
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

const size_t nixlOfiEngine::NUM_SUPPORTED_PROVIDERS =
    sizeof(SUPPORTED_PROVIDERS) / sizeof(SUPPORTED_PROVIDERS[0]);

const nixlOfiEngine::ProviderConfig* nixlOfiEngine::findProviderConfig(const std::string& provider_name) {
    for (size_t i = 0; i < NUM_SUPPORTED_PROVIDERS; ++i) {
        if (SUPPORTED_PROVIDERS[i].name == provider_name) {
            return &SUPPORTED_PROVIDERS[i];
        }
    }
    return nullptr;
}

void nixlOfiEngine::configureHintsForProvider(struct fi_info* hints, const std::string& provider_name) {
    const auto* config = findProviderConfig(provider_name);

    if (!config) {
        // if the provider is not in our list, use the verbs config as a safe default
        config = findProviderConfig("verbs");
        NIXL_DEBUG << "Unknown provider '" << provider_name << "', using verbs config as a fallback.";
    } else {
        NIXL_DEBUG << "Using predefined config for provider: " << provider_name;
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
    // other tx_attr fields can be added here as needed
    
    // rx_attr fields can be added here as needed

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

nixlOfiEngine::~nixlOfiEngine() {
    if (!isConnectionless_) {
        eqThreadStop_ = true;
        if (eqThread_.joinable()) {
            // wake up the EQ thread to ensure it exits
            if (eq_) {
                uint32_t event;
                fi_eq_read(eq_, &event, nullptr, 0, 0);
            }
            eqThread_.join();
        }
    }

    // close connected endpoints
    for (auto const& [key, val] : connectedEps_) {
        fi_close(&val->fid);
    }

    if (pep_)    { fi_close(&pep_->fid);    pep_ = nullptr; }
    if (ep_)     { fi_close(&ep_->fid);     ep_ = nullptr; }
    if (cq_)     { fi_close(&cq_->fid);     cq_ = nullptr; }
    if (eq_)     { fi_close(&eq_->fid);     eq_ = nullptr; }
    if (av_)     { fi_close(&av_->fid);     av_ = nullptr; }
    if (domain_) { fi_close(&domain_->fid); domain_ = nullptr; }
    if (fabric_) { fi_close(&fabric_->fid); fabric_ = nullptr; }
    if (cachedProviderInfo_) fi_freeinfo(cachedProviderInfo_);

    // note: static handles are shared across instances
    // cleanup is handled by OS when process exits
}

bool nixlOfiEngine::supportsNotif() const {
    return true;
}

bool nixlOfiEngine::supportsRemote() const {
    return true;
}

bool nixlOfiEngine::supportsLocal() const {
    return false;
}

bool nixlOfiEngine::supportsProgTh() const {
    return true;
}

nixl_status_t nixlOfiEngine::getNotifs(notif_list_t &notif_list) {
    if (!notif_list.empty()) {
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // TODO: Implement actual OFI notification mechanism using fi_cq_read or fi_eq_read
    // For now, return empty list since OFI notifications are not yet implemented
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    // TODO: Implement actual OFI notification sending mechanism
    // This could use fi_send with a special notification message format
    // For now, return success as a no-op to satisfy the interface
    NIXL_DEBUG << "OFI genNotif stub called for agent " << remote_agent << " with message: " << msg;
    return NIXL_SUCCESS;
}

nixl_mem_list_t nixlOfiEngine::getSupportedMems() const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    if (hmemCudaSupported_) {
        mems.push_back(VRAM_SEG);
    }
    if (hmemSynapseaiSupported_) {
        mems.push_back(VRAM_SEG);
    }
    if (hmemZeSupported_) {
        mems.push_back(VRAM_SEG);
    }
    return mems;
}

nixl_status_t nixlOfiEngine::connect(const std::string &remote_agent) {
    NIXL_DEBUG << "connect() called for remote_agent: " << remote_agent 
               << " isConnectionless: " << isConnectionless_;
    std::lock_guard<std::mutex> lock(epLock_);
    return connect_unlocked(remote_agent);
}

nixl_status_t nixlOfiEngine::connect_unlocked(const std::string &remote_agent) {
    // Note: epLock_ must already be held by caller
    NIXL_DEBUG << "connect_unlocked() called for remote_agent: " << remote_agent 
               << " isConnectionless: " << isConnectionless_;

    if (isConnectionless_) {
        // for connectionless providers like shm: insert remote address into av
        if (avAddrs_.count(remote_agent)) {
            NIXL_DEBUG << "Already have address mapping for " << remote_agent;
            return NIXL_SUCCESS;
        }

        auto remote_addr_it = remoteAddrs_.find(remote_agent);
        if (remote_addr_it == remoteAddrs_.end()) {
            NIXL_ERROR << "Remote address for " << remote_agent << " not found.";
            return NIXL_ERR_NOT_FOUND;
        }

        fi_addr_t addr = FI_ADDR_UNSPEC;
        // Debug: dump remote address blob before calling fi_av_insert
        {
            const std::string &r = remote_addr_it->second;
            std::string dump;
            dump.reserve(r.size() * 3);
            for (unsigned char c : r) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x", c);
                dump.append(buf);
                dump.push_back(' ');
            }
            NIXL_DEBUG << "Attempting fi_av_insert for remote_agent=" << remote_agent
                       << " addr_len=" << r.size() << " hex=" << dump;
        }
        int ret = fi_av_insert(av_, remote_addr_it->second.data(), 1, &addr, 0, nullptr);
        if (ret != 1) {
            NIXL_ERROR << "fi_av_insert failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        avAddrs_[remote_agent] = addr;
        NIXL_DEBUG << "fi_av_insert success: fi_addr=" << addr << " (ret=" << ret << ")";
        return NIXL_SUCCESS;
    }

    // connection-oriented logic
    if (connectedEps_.count(remote_agent)) {
        NIXL_DEBUG << "Already connected to " << remote_agent;
        return NIXL_SUCCESS;
    }

    auto remote_addr_it = remoteAddrs_.find(remote_agent);
    if (remote_addr_it == remoteAddrs_.end()) {
        NIXL_ERROR << "Remote address for " << remote_agent << " not found.";
        return NIXL_ERR_NOT_FOUND;
    }
    const std::string &remote_addr_str = remote_addr_it->second;

    // create copy of provider info to avoid shared state issues
    struct fi_info *remote_fi = fi_dupinfo(cachedProviderInfo_);
    if (!remote_fi) {
        NIXL_ERROR << "Failed to duplicate provider info for remote agent";
        return NIXL_ERR_BACKEND;
    }

    // update dest_addr for this connection
    remote_fi->dest_addr = (void*)remote_addr_str.c_str();
    remote_fi->dest_addrlen = remote_addr_str.length();
    
    
    // let libfabric use the auto-negotiated address format from fi_getinfo

    fid_ep *remote_ep = nullptr;
    int ret = fi_endpoint(domain_, remote_fi, &remote_ep, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_endpoint for remote failed: " << fi_strerror(-ret);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(remote_ep, &cq_->fid, FI_SEND | FI_RECV);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to CQ for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    // use no flags for all providers to avoid compatibility issues
    ret = fi_ep_bind(remote_ep, &eq_->fid, 0);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to EQ for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_enable(remote_ep);
    if (ret) {
        NIXL_ERROR << "fi_enable for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    // pause the event loop to prevent it from consuming our FI_CONNECTED event
    eqThreadPaused_.store(true);
    // Give the event loop time to notice the pause and stop processing
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    ret = fi_connect(remote_ep, remote_fi->dest_addr, localAgentName_.c_str(), localAgentName_.length() + 1);
    if (ret) {
        NIXL_ERROR << "fi_connect failed: " << fi_strerror(-ret);
        // resume event loop before returning
        eqThreadPaused_.store(false);
        eqPauseCV_.notify_one();
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    // wait for connection to complete via EQ
    // use fi_eq_sread (similar to fabtests implementation)
    // for blocking synchronous read to avoid race with event loop
    struct fi_eq_cm_entry entry;
    uint32_t event;
    ssize_t n_events = fi_eq_sread(eq_, &event, &entry, sizeof(entry), -1, 0);
    
    // resume event loop now that we got our event
    eqThreadPaused_.store(false);
    eqPauseCV_.notify_one();
    
    if (n_events != sizeof(entry)) {
        if (n_events < 0) {
            NIXL_ERROR << "fi_eq_sread failed during connect: " << fi_strerror(-n_events);
            // Try to read error details if available
            if (n_events == -FI_EAVAIL) {
                struct fi_eq_err_entry err_entry;
                ssize_t err_ret = fi_eq_readerr(eq_, &err_entry, 0);
                if (err_ret == sizeof(err_entry)) {
                    NIXL_ERROR << "EQ error details: prov_errno=" << err_entry.prov_errno 
                               << " err=" << err_entry.err << " (" << fi_strerror(err_entry.err) << ")";
                }
            }
        } else {
            NIXL_ERROR << "fi_eq_sread returned unexpected size: " << n_events << " (expected " << sizeof(entry) << ")";
        }
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }
    if (event != FI_CONNECTED || entry.fid != &remote_ep->fid) {
        NIXL_ERROR << "Unexpected EQ event during connect: " << event << " (expected FI_CONNECTED=" << FI_CONNECTED << ")";
        fi_close(&remote_ep->fid);
        fi_freeinfo(remote_fi);
        return NIXL_ERR_BACKEND;
    }

    connectedEps_[remote_agent] = remote_ep;
    fi_freeinfo(remote_fi);

    NIXL_DEBUG << "OFI backend: Connected to " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::disconnect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(epLock_);

    if (isConnectionless_) {
        // connectionless provider, remove address mapping
        auto it = avAddrs_.find(remote_agent);
        if (it == avAddrs_.end()) {
            NIXL_WARN << "OFI backend: No address mapping for " << remote_agent;
            return NIXL_ERR_NOT_FOUND;
        }

        int ret = fi_av_remove(av_, &it->second, 1, 0);
        if (ret) {
            NIXL_ERROR << "fi_av_remove failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        avAddrs_.erase(it);
        NIXL_DEBUG << "OFI backend: Removed address mapping for " << remote_agent;
        return NIXL_SUCCESS;
    }

    // connection-oriented case
    auto it = connectedEps_.find(remote_agent);
    if (it == connectedEps_.end()) {
        NIXL_WARN << "OFI backend: No active connection to " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    // Store endpoint before erasing to ensure proper cleanup even if fi_close fails
    fid_ep* ep_to_close = it->second;
    connectedEps_.erase(it);
    
    int ret = fi_close(&ep_to_close->fid);
    if (ret) {
        NIXL_ERROR << "fi_close (remote_ep) failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }
    NIXL_DEBUG << "OFI backend: Disconnected from " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::registerMem(const nixlBlobDesc &mem,
                                     const nixl_mem_t &nixl_mem,
                                     nixlBackendMD* &out) {
    nixlOfiMetadata *ofi_meta = new nixlOfiMetadata();
    if (!ofi_meta) {
        return NIXL_ERR_BACKEND;
    }

    nixl_status_t status = NIXL_SUCCESS;
    
    if (nixl_mem == DRAM_SEG) {
        status = registerDramMemory(mem, ofi_meta);
    } else if (nixl_mem == VRAM_SEG) {
        status = registerVramMemory(mem, ofi_meta);
    } else {
        NIXL_ERROR << "Unsupported memory type: " << nixl_mem;
        delete ofi_meta;
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (status != NIXL_SUCCESS) {
        delete ofi_meta;
        return status;
    }

    if (!ofi_meta->mr) {
        NIXL_ERROR << "Memory registration returned null mr";
        delete ofi_meta;
        return NIXL_ERR_BACKEND;
    }
    
    ofi_meta->desc = fi_mr_desc(ofi_meta->mr);
    if (!ofi_meta->desc) {
        NIXL_ERROR << "fi_mr_desc failed";
        fi_close(&ofi_meta->mr->fid);
        delete ofi_meta;
        return NIXL_ERR_BACKEND;
    }

    out = ofi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::deregisterMem(nixlBackendMD *meta) {
    nixlOfiMetadata *ofi_meta = static_cast<nixlOfiMetadata*>(meta);
    if (!ofi_meta) {
        return NIXL_ERR_INVALID_PARAM;
    }

    // Only close mr for local metadata - remote metadata has mr = nullptr
    if (ofi_meta->mr) {
        int ret = fi_close(&ofi_meta->mr->fid);
        if (ret) {
            NIXL_ERROR << "fi_close (mr) failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }
    }

    delete ofi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::unloadMD(nixlBackendMD* input) {
    return deregisterMem(input);
}

nixl_status_t nixlOfiEngine::prepXfer(const nixl_xfer_op_t &operation,
                                  const nixl_meta_dlist_t &local,
                                  const nixl_meta_dlist_t &remote,
                                  const std::string &remote_agent,
                                  nixlBackendReqH* &handle,
                                  const nixl_opt_b_args_t* opt_args) const {
    // validate parameters upfront
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "mismatched descriptor counts: local=" << local.descCount()
                   << ", remote=" << remote.descCount();
        return NIXL_ERR_INVALID_PARAM;
    }
    
    if (local.descCount() <= 0) {
        NIXL_ERROR << "no descriptors to transfer";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // for connectionless providers, address resolution happens in postXfer
    // for connection-oriented providers, connection validation happens in postXfer
    
    // validate all transfer parameters
    for (size_t i = 0; i < static_cast<size_t>(local.descCount()); ++i) {
        nixl_status_t status = validateTransferParams(local[i], remote[i]);
        if (status != NIXL_SUCCESS) {
            NIXL_ERROR << "validation failed for descriptor " << i;
            return status;
        }
    }
    
    // create request handle
    nixlOfiRequest *ofi_req = new nixlOfiRequest();
    if (!ofi_req) {
        return NIXL_ERR_BACKEND;
    }
    
    // store parameters for later execution
    ofi_req->cq = cq_;
    NIXL_DEBUG << "prepXfer: created request with cq=" << ofi_req->cq << " (engine cq=" << cq_ << ")";
    ofi_req->operation = operation;
    ofi_req->remote_agent = remote_agent;
    ofi_req->total_operations = static_cast<size_t>(local.descCount());
    ofi_req->completed_operations = 0;
    ofi_req->is_prepared = true;
    ofi_req->is_posted = false;
    
    // copy descriptors
    ofi_req->local_descs.reserve(ofi_req->total_operations);
    ofi_req->remote_descs.reserve(ofi_req->total_operations);
    
    for (size_t i = 0; i < ofi_req->total_operations; ++i) {
        ofi_req->local_descs.push_back(local[i]);
        ofi_req->remote_descs.push_back(remote[i]);
    }
    
    handle = ofi_req;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::postXfer(const nixl_xfer_op_t &operation,
                                  const nixl_meta_dlist_t &local,
                                  const nixl_meta_dlist_t &remote,
                                  const std::string &remote_agent,
                                  nixlBackendReqH* &handle,
                                  const nixl_opt_b_args_t* opt_args) const {
    
    // handle prepared requests
    if (handle) {
        nixlOfiRequest *ofi_req = static_cast<nixlOfiRequest*>(handle);
        if (!ofi_req->is_prepared) {
            NIXL_ERROR << "request not prepared";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (ofi_req->is_posted) {
            NIXL_ERROR << "request already posted";
            return NIXL_ERR_INVALID_PARAM;
        }
        return postPreparedXfer(ofi_req);
    }
    
    // legacy path: prepare and post immediately
    nixl_status_t prep_status = prepXfer(operation, local, remote, remote_agent, handle, opt_args);
    if (prep_status != NIXL_SUCCESS) {
        return prep_status;
    }
    
    return postPreparedXfer(static_cast<nixlOfiRequest*>(handle));
}
nixl_status_t nixlOfiEngine::postPreparedXfer(nixlOfiRequest* ofi_req) const {
    if (!ep_) {
        NIXL_ERROR << "primary endpoint not initialized";
        return NIXL_ERR_BACKEND;
    }
    
    if (!ofi_req || !ofi_req->is_prepared) {
        NIXL_ERROR << "invalid or unprepared request";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // get target endpoint and destination address
    fid_ep *target_ep = ep_;
    fi_addr_t dest_addr = FI_ADDR_UNSPEC;
    
    std::lock_guard<std::mutex> lock(epLock_);  // protect address/connection maps
    
    if (isConnectionless_) {
        // for connectionless (RDM/DGRAM), use address vector
        auto av_it = avAddrs_.find(ofi_req->remote_agent);
        if (av_it == avAddrs_.end()) {
            NIXL_ERROR << "no AV address mapping for " << ofi_req->remote_agent << " (connectionless)";
            NIXL_ERROR << "available mappings: " << avAddrs_.size();
            for (const auto& pair : avAddrs_) {
                NIXL_ERROR << "  " << pair.first;
            }
            return NIXL_ERR_NOT_FOUND;
        }
        dest_addr = av_it->second;
        NIXL_DEBUG << "using AV address " << dest_addr << " for " << ofi_req->remote_agent;
    } else {
        // for connection-oriented (MSG), use connected endpoint
        auto it = connectedEps_.find(ofi_req->remote_agent);
        if (it == connectedEps_.end()) {
            NIXL_ERROR << "not connected to " << ofi_req->remote_agent << " (connection-oriented)";
            return NIXL_ERR_NOT_FOUND;
        }
        target_ep = it->second;
        if (!target_ep) {
            NIXL_ERROR << "connected endpoint is null for " << ofi_req->remote_agent;
            return NIXL_ERR_BACKEND;
        }
        NIXL_DEBUG << "using connected endpoint for " << ofi_req->remote_agent;
    }

    // prepare contexts for operations
    ofi_req->op_contexts.reserve(ofi_req->total_operations);
    
    // post all operations
    int ret = 0;
    for (size_t i = 0; i < ofi_req->total_operations; ++i) {
        const nixlMetaDesc &local_desc = ofi_req->local_descs[i];
        const nixlMetaDesc &remote_desc = ofi_req->remote_descs[i];

        nixlOfiMetadata *local_meta = static_cast<nixlOfiMetadata*>(local_desc.metadataP);
        nixlOfiMetadata *remote_meta = static_cast<nixlOfiMetadata*>(remote_desc.metadataP);
        
        // parameters already validated in prepXfer

        // get remote memory key
        uint64_t remote_key = getRemoteKey(remote_meta);
        NIXL_DEBUG << "Remote key extraction: mr=" << remote_meta->mr << " key=" << remote_key;
        if (remote_meta->mr) {
            NIXL_DEBUG << "  using fi_mr_key, calculated key=" << fi_mr_key(remote_meta->mr);
        } else {
            NIXL_DEBUG << "  using desc field, desc=" << remote_meta->desc;
        }
        
        struct fi_rma_iov rma_iov = {
            .addr = (uint64_t)remote_desc.addr,
            .len = remote_desc.len,
            .key = remote_key
        };

        // create unique context for operation
        auto op_context = std::make_unique<uint64_t>(i);
        uint64_t* raw_context = op_context.get();
        NIXL_DEBUG << "postXfer: operation " << i << " context=" << raw_context << " value=" << *raw_context;
        
        NIXL_DEBUG << "RMA operation parameters:";
        NIXL_DEBUG << "  local_addr=0x" << std::hex << local_desc.addr << " len=" << std::dec << local_desc.len;
        NIXL_DEBUG << "  remote_addr=0x" << std::hex << rma_iov.addr << " key=" << rma_iov.key;
        NIXL_DEBUG << "  dest_addr=" << dest_addr << " context=" << raw_context;
        NIXL_DEBUG << "  local_desc=" << local_meta->desc;
        
        switch (ofi_req->operation) {
            case NIXL_READ:
                ret = fi_read(target_ep, reinterpret_cast<void*>(local_desc.addr),
                             local_desc.len, local_meta->desc, dest_addr,
                             rma_iov.addr, rma_iov.key, raw_context);
                NIXL_DEBUG << "fi_read returned: " << ret << " (" << fi_strerror(-ret) << ")";
                break;
            case NIXL_WRITE:
                ret = fi_write(target_ep, reinterpret_cast<void*>(local_desc.addr),
                              local_desc.len, local_meta->desc, dest_addr,
                              rma_iov.addr, rma_iov.key, raw_context);
                break;
            default:
                NIXL_ERROR << "unsupported operation type";
                return NIXL_ERR_NOT_SUPPORTED;
        }

        if (ret && ret != -FI_EAGAIN) {
            NIXL_ERROR << "operation " << i << " failed: " << fi_strerror(-ret);
            NIXL_ERROR << "successfully posted " << ofi_req->op_contexts.size() << " operations before failure";
            // update total operations to match what was actually posted
            ofi_req->total_operations = ofi_req->op_contexts.size();
            if (ofi_req->total_operations > 0) {
                ofi_req->is_posted = true;  // mark as posted for cleanup
                NIXL_ERROR << "partial posting: " << ofi_req->total_operations << " operations posted";
            }
            return NIXL_ERR_BACKEND;
        }
        
        // operation posted successfully (ret == 0 or ret == -FI_EAGAIN)
        ofi_req->op_contexts.push_back(std::move(op_context));
        
        if (ret == -FI_EAGAIN) {
            NIXL_DEBUG << "operation " << i << " posted asynchronously";
        }
    }

    // mark as posted
    ofi_req->is_posted = true;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::checkXfer(nixlBackendReqH* handle) const {
    nixlOfiRequest *ofi_req = static_cast<nixlOfiRequest*>(handle);
    if (!ofi_req || !ofi_req->cq) {
        NIXL_ERROR << "checkXfer: invalid request or CQ";
        return NIXL_ERR_INVALID_PARAM;
    }

    if (!ofi_req->is_posted) {
        NIXL_ERROR << "checkXfer: request not posted yet";
        return NIXL_ERR_INVALID_PARAM; // request not posted yet
    }

    NIXL_DEBUG << "checkXfer: completed=" << ofi_req->completed_operations 
              << " total=" << ofi_req->total_operations;

    if (ofi_req->isComplete()) {
        NIXL_DEBUG << "checkXfer: all operations completed";
        return NIXL_SUCCESS;
    }

    // read available completions
    const size_t batch_size = 16;
    // struct fi_cq_data_entry entries[batch_size];
    struct fi_cq_entry entries[batch_size];
    int ret = fi_cq_read(ofi_req->cq, entries, batch_size);
    
    NIXL_DEBUG << "checkXfer: fi_cq_read returned " << ret;
    
    if (ret > 0) {
        // update completion count
        ofi_req->completed_operations += static_cast<size_t>(ret);
        NIXL_DEBUG << "checkXfer: got " << ret << " completions, total completed=" 
                  << ofi_req->completed_operations << "/" << ofi_req->total_operations;
        
        // log completion contexts for debugging
        for (int i = 0; i < ret; ++i) {
            NIXL_DEBUG << "checkXfer: completion " << i << " context=" << entries[i].op_context;
        }
        
        if (ofi_req->isComplete()) {
            NIXL_DEBUG << "checkXfer: all operations completed - SUCCESS";
            return NIXL_SUCCESS;
        }
        return NIXL_IN_PROG;
    } else if (ret == -FI_EAGAIN) {
        NIXL_DEBUG << "checkXfer: no completions available (EAGAIN)";
        return NIXL_IN_PROG;
    } else if (ret < 0) {
        NIXL_ERROR << "checkXfer: CQ error, calling handleCQError";
        return handleCQError(ofi_req->cq, ret);
    }
    
    return NIXL_IN_PROG;
}

#if 0
nixl_status_t nixlOfiEngine::checkXfer(nixlBackendReqH* handle) const {
    auto* ofi_req = static_cast<ofi_req_t*>(handle);  // your context type
    if (!ofi_req) return NIXL_ERR_INVAL;

    // drain up to N completions per call
    constexpr size_t batch = 32;
    struct fi_cq_entry entries[batch];

    for (;;) {
        ssize_t rc = fi_cq_read(cq_, entries, batch);

        if (rc > 0) {
            for (ssize_t i = 0; i < rc; ++i) {
                void* ctx = entries[i].op_context;
                auto* done_req = static_cast<ofi_req_t*>(ctx);
                if (!done_req) {
                    NIXL_ERROR << "CQ returned null op_context";
                    return NIXL_ERR_BACKEND;
                }
                // decrement pending and mark completion
                auto left = done_req->pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (left == 0) {
                    done_req->complete.store(true, std::memory_order_release);
                }
            }
            // keep draining until CQ is empty this call
            continue;
        }

        if (rc == -FI_EAGAIN) {
            // no more completions now
            break;
        }

        // rc < 0 and not EAGAIN: pull error entry for details
        struct fi_cq_err_entry err = {};
        int erc = fi_cq_readerr(cq_, &err, 0);
        if (erc > 0) {
            const char* emsg = fi_strerror(err.err);
            NIXL_ERROR << "CQ error: " << emsg
                       << " prov_err=" << err.prov_errno
                       << " ctx=" << err.op_context;
            // best effort: decrement the request we find in err.op_context
            if (err.op_context) {
                auto* bad_req = static_cast<ofi_req_t*>(err.op_context);
                auto left = bad_req->pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
                bad_req->failed.store(true, std::memory_order_release);
                if (left == 0) bad_req->complete.store(true, std::memory_order_release);
            }
            return NIXL_ERR_BACKEND;
        } else {
            NIXL_ERROR << "fi_cq_read failed: " << fi_strerror(-rc);
            return NIXL_ERR_BACKEND;
        }
    }

    // report status of the specific handle we were asked to check
    if (ofi_req->failed.load(std::memory_order_acquire)) return NIXL_ERR_BACKEND;
    return ofi_req->complete.load(std::memory_order_acquire) ? NIXL_OK : NIXL_IN_PROG;
}
#endif

nixl_status_t nixlOfiEngine::releaseReqH(nixlBackendReqH* handle) const {
    nixlOfiRequest *ofi_req = static_cast<nixlOfiRequest*>(handle);
    if (!ofi_req) {
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // try to drain pending completions briefly
    if (ofi_req->is_posted && !ofi_req->isComplete()) {
        int drain_attempts = 0;
        const int max_drain_attempts = 5;
        
        while (!ofi_req->isComplete() && drain_attempts < max_drain_attempts) {
            nixl_status_t status = checkXfer(handle);
            if (status == NIXL_ERR_BACKEND) {
                break;
            }
            drain_attempts++;
            
            if (!ofi_req->isComplete()) {
                usleep(100);
            }
        }
        
        if (!ofi_req->isComplete()) {
            NIXL_DEBUG << "releasing request with " << ofi_req->remainingOperations() 
                      << " pending operations";
        }
    }
    
    // contexts cleaned up automatically by unique_ptr destructors
    delete ofi_req;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getConnInfo(std::string &conn_info) const {
    conn_info = localAddr_;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) {
    // validate remote agent name to prevent memory attacks
    if (remote_agent.empty() || remote_agent.size() > 256) {
        NIXL_ERROR << "Invalid remote agent name length: " << remote_agent.size();
        return NIXL_ERR_INVALID_PARAM;
    }
    
    if (conn_info.empty() || conn_info.size() > 1024) {
        NIXL_ERROR << "Invalid connection info size: " << conn_info.size();
        return NIXL_ERR_INVALID_PARAM;
    }
    
    std::lock_guard<std::mutex> lock(epLock_);
    NIXL_DEBUG << "loadRemoteConnInfo: storing conn_info for " << remote_agent << " size=" << conn_info.size();
    remoteAddrs_[remote_agent] = conn_info;
    
    // CRITICAL FIX: Establish connection immediately when remote agent info is loaded
    // This prevents data integrity issues caused by auto-connect during first transfer
    NIXL_DEBUG << "Establishing connection to " << remote_agent << " immediately";
    nixl_status_t connect_status = connect_unlocked(remote_agent);
    if (connect_status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to establish connection to " << remote_agent << " during loadRemoteConnInfo";
        // Remove the address entry since connection failed
        remoteAddrs_.erase(remote_agent);
        return connect_status;
    }
    
    NIXL_DEBUG << "Successfully established connection to " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getPublicData(const nixlBackendMD* meta, std::string &str) const {
    const nixlOfiMetadata* ofi_meta = static_cast<const nixlOfiMetadata*>(meta);
    if (!ofi_meta || !ofi_meta->mr) {
        NIXL_ERROR << "getPublicData: invalid metadata - ofi_meta=" << ofi_meta << " mr=" << (ofi_meta ? ofi_meta->mr : nullptr);
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // serialize memory registration key for remote access
    uint64_t mr_key = fi_mr_key(ofi_meta->mr);
    str = std::to_string(mr_key);
    NIXL_ERROR << "getPublicData: mr_key=" << mr_key << " str=" << str;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteMD(const nixlBlobDesc &input, const nixl_mem_t &nixl_mem,
                                           const std::string &remote_agent, nixlBackendMD* &output) {
    // create a remote metadata object from the serialized public data
    nixlOfiMetadata* remote_meta = new nixlOfiMetadata();
    if (!remote_meta) {
        return NIXL_ERR_BACKEND;
    }
    
    // validate input metadata
    if (input.metaInfo.empty() || input.metaInfo.size() > 32) {
        delete remote_meta;
        NIXL_ERROR << "Invalid metadata size: " << input.metaInfo.size();
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // parse the memory key from the metadata string
    try {
        // copy metaInfo to ensure null termination and lifetime safety
        std::string key_str(input.metaInfo.begin(), input.metaInfo.end());
        uint64_t remote_key = std::stoull(key_str);
        
        // validate remote key before storing
        // Note: key=0 is valid for some providers (e.g., TCP) that don't use memory keys
        if (remote_key == UINT64_MAX) {
            delete remote_meta;
            NIXL_ERROR << "Invalid remote memory key: " << remote_key;
            return NIXL_ERR_INVALID_PARAM;
        }
        
        // for remote metadata, we don't have an actual mr object, just the key
        // store the key for later use in RMA operations - safe conversion
        remote_meta->mr = nullptr;  // No local mr for remote metadata
        remote_meta->desc = reinterpret_cast<void*>(static_cast<uintptr_t>(remote_key));
        
        output = remote_meta;
        return NIXL_SUCCESS;
    } catch (...) {
        delete remote_meta;
        NIXL_ERROR << "Failed to parse remote memory key";
        return NIXL_ERR_INVALID_PARAM;
    }
}

void nixlOfiEngine::eq_event_loop() {
    while (!eqThreadStop_) {
        // check if we need to pause the event loop during client connections
        if (eqThreadPaused_.load()) {
            std::unique_lock<std::mutex> lock(eqPauseMutex_);
            eqPauseCV_.wait(lock, [this] { return !eqThreadPaused_.load() || eqThreadStop_.load(); });
            if (eqThreadStop_) {
                break;
            }
        }
        
        struct fi_eq_cm_entry entry;
        uint32_t event;
        ssize_t ret = fi_eq_read(eq_, &event, &entry, 1, eqTimeoutMs_);

        if (ret == -FI_EAGAIN) {
            continue;
        } else if (ret < 0) {
            if (ret == -FI_EINTR && eqThreadStop_) {
                // interrupt
                break;
            }
            NIXL_ERROR << "fi_eq_read failed in event loop: " << fi_strerror(-ret);
            // TODO: error handling
            continue;
        }

        switch (event) {
            case FI_CONNREQ:
            {
                NIXL_DEBUG << "FI_CONNREQ event received";
                fid_ep *new_ep = nullptr;

                // accept
                int connreq_ret = fi_endpoint(domain_, fi_, &new_ep, nullptr);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_endpoint for accepted connection failed: " << fi_strerror(-connreq_ret);
                    break;
                }
                connreq_ret = fi_ep_bind(new_ep, &cq_->fid, FI_SEND | FI_RECV);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_ep_bind to CQ for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                // use no flags for EQ binding to avoid compatibility issues
                connreq_ret = fi_ep_bind(new_ep, &eq_->fid, 0);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_ep_bind to EQ for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                connreq_ret = fi_accept(new_ep, nullptr, 0);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_accept failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                connreq_ret = fi_enable(new_ep);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_enable for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }

                std::string remote_agent_name = "connected_agent_" + std::to_string(reinterpret_cast<uintptr_t>(new_ep));

                std::lock_guard<std::mutex> lock(epLock_);
                connectedEps_[remote_agent_name] = new_ep;
                NIXL_DEBUG << "Accepted connection from " << remote_agent_name;
                break;
            }
            case FI_CONNECTED:
                NIXL_DEBUG << "FI_CONNECTED event received for outgoing connection";
                // TODO: async model
                break;
            case FI_SHUTDOWN:
                NIXL_DEBUG << "FI_SHUTDOWN event received";
                {
                    std::lock_guard<std::mutex> lock(epLock_);
                    for (auto it = connectedEps_.begin(); it != connectedEps_.end(); ++it) {
                        if (&it->second->fid == entry.fid) {
                            fi_close(&it->second->fid);
                            connectedEps_.erase(it);
                            break;
                        }
                    }
                }
                break;
            default:
                NIXL_WARN << "Unhandled EQ event: " << event;
                break;
        }
    }
}


nixl_status_t nixlOfiEngine::setupEndpoint(bool use_passive_endpoint) {
    int ret = 0;

    // create endpoint
    ret = fi_endpoint(domain_, fi_, &ep_, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_endpoint failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND; // ep_ was never created, no cleanup needed
    }

    // create and bind completion queue
    struct fi_cq_attr cq_attr = {};
    cq_attr.size = 128; // use fi_->tx_attr->size + fi_->rx_attr->size?
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    ret = fi_cq_open(domain_, &cq_attr, &cq_, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_cq_open failed: " << fi_strerror(-ret);
        goto cleanup_setup;
    }

    // bind cq to endpoint
    ret = fi_ep_bind(ep_, &cq_->fid, FI_SEND | FI_RECV);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to CQ failed: " << fi_strerror(-ret);
        goto cleanup_setup;
    }

    if (use_passive_endpoint) {
        // event queue for connection management
        struct fi_eq_attr eq_attr = {};
        eq_attr.size = 64;
        eq_attr.wait_obj = FI_WAIT_UNSPEC;
        ret = fi_eq_open(fabric_, &eq_attr, &eq_, nullptr);
        if (ret) {
            NIXL_ERROR << "fi_eq_open failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }

        // bind endpoint to EQ for connection management
        ret = fi_ep_bind(ep_, &eq_->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_ep_bind to EQ failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }

        // create passive endpoint for listening
        ret = fi_passive_ep(fabric_, fi_, &pep_, nullptr);
        if (ret) {
            const char* prov = (fi_ && fi_->fabric_attr && fi_->fabric_attr->prov_name)
                               ? fi_->fabric_attr->prov_name : "unknown";
            NIXL_ERROR << "fi_passive_ep failed on provider=" << prov
                       << " ep_type=" << fi_tostr(&fi_->ep_attr->type, FI_TYPE_EP_TYPE)
                       << " err=" << -ret << " (" << fi_strerror(-ret) << ")";
            goto cleanup_setup;
        }

        ret = fi_pep_bind(pep_, &eq_->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_pep_bind to EQ failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }

        ret = fi_listen(pep_);
        if (ret) {
            NIXL_ERROR << "fi_listen failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }
        
        // enable the endpoint for connection-oriented
        ret = fi_enable(ep_);
        if (ret) {
            NIXL_ERROR << "fi_enable failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }
        
        NIXL_DEBUG << "connection-oriented endpoint listening for connections";
    } else {
        // address vector for connectionless communication
        struct fi_av_attr av_attr = {};
        av_attr.type = FI_AV_TABLE;
        ret = fi_av_open(domain_, &av_attr, &av_, nullptr);
        if (ret) {
            NIXL_ERROR << "fi_av_open failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }

        ret = fi_ep_bind(ep_, &av_->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_ep_bind to AV failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }

        ret = fi_enable(ep_);
        if (ret) {
            NIXL_ERROR << "fi_enable failed: " << fi_strerror(-ret);
            goto cleanup_setup;
        }
    }
    return NIXL_SUCCESS;

cleanup_setup:
    // cleanup only what setupEndpoint created, set pointers to nullptr
    // Close endpoint BEFORE CQ since EP depends on CQ
    if (ep_)  { fi_close(&ep_->fid);  ep_ = nullptr; }
    if (av_)  { fi_close(&av_->fid);  av_ = nullptr; }
    if (pep_) { fi_close(&pep_->fid); pep_ = nullptr; }
    if (eq_)  { fi_close(&eq_->fid);  eq_ = nullptr; }
    if (cq_)  { fi_close(&cq_->fid);  cq_ = nullptr; }
    return NIXL_ERR_BACKEND;
}

nixl_status_t nixlOfiEngine::getEndpointAddress(fid_ep* endpoint, std::string& address) {
    if (!endpoint) {
        return NIXL_ERR_INVALID_PARAM;
    }

    size_t addrlen = 256;
    std::vector<char> addr_buf(addrlen);
    int ret = fi_getname(&endpoint->fid, addr_buf.data(), &addrlen);
    if (ret) {
        NIXL_ERROR << "fi_getname failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    address = std::string(addr_buf.data(), addrlen);
    return NIXL_SUCCESS;
}

void nixlOfiEngine::detectHmemCapabilities(struct fi_info* fi_info,
                                            const std::string& provider_name,
                                            bool& cuda_supported,
                                            bool& ze_supported,
                                            bool& synapseai_supported) {
    // Check if provider supports generic HMEM capability
    if (!fi_info || !(fi_info->caps & FI_HMEM)) {
        NIXL_DEBUG << "Provider " << provider_name << " does not support generic HMEM";
        
        // Special cases: providers that support SynapseAI through DMA buffers
        // even without advertising FI_HMEM capability
        if (provider_name == "verbs" || provider_name == "verbs;ofi_rxm") {
            synapseai_supported = true;
        } else {
            synapseai_supported = false;
        }
        
        cuda_supported = false;
        ze_supported = false;
        return;
    }

    NIXL_DEBUG << "Provider " << provider_name << " supports generic HMEM capability";

    // for now, conservatively enable all interfaces if HMEM is supported
    // TODO: determine which specific interfaces actually work
    cuda_supported = true;
    ze_supported = true; 
    synapseai_supported = true;

    NIXL_DEBUG << "HMEM interfaces marked as potentially available - runtime detection will validate";
}

uint64_t nixlOfiEngine::getMemoryRegistrationAccessFlags(const struct fi_info* fi_info) {
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

fi_hmem_iface nixlOfiEngine::selectHmemInterface(const nixlBlobDesc &mem, uint64_t &device_id) const {
    device_id = mem.devId >= 0 ? mem.devId : 0;
    
    // helper for safe environment variable checking
    auto isEnvTrue = [](const char* env_val) -> bool {
        return env_val && (strcmp(env_val, "1") == 0 || strcmp(env_val, "true") == 0);
    };
    
    // Synapse device validation
    auto validateSynapseAIDevice = [](uint64_t dev_id) -> bool {
        std::string device_path = "/dev/accel/accel" + std::to_string(dev_id);
        if (access(device_path.c_str(), R_OK | W_OK) != 0) {
            NIXL_INFO << "SynapseAI device " << device_path << " not accessible, will fallback to system memory";
            return false;
        }
        return true;
    };
    
    auto validateCudaDevice = [](uint64_t dev_id) -> bool {
        // TODO: add proper CUDA device validation
        return true;
    };
    
    auto validateZeDevice = [](uint64_t dev_id) -> bool {
        // TODO: add proper ZE device validation
        return true;
    };
    
    // HMEM interface configuration
    struct HmemConfig {
        fi_hmem_iface iface;
        bool supported;
        const char* name;
        const char* explicit_env;      // HMEM_SYNAPSEAI, HMEM_CUDA, etc.
        const char* implicit_env;      // HABANA_VISIBLE_DEVICES, CUDA_VISIBLE_DEVICES, etc.
        std::function<bool(uint64_t)> validate;
    };
    
    const HmemConfig configs[] = {
        {
            FI_HMEM_SYNAPSEAI, 
            hmemSynapseaiSupported_, 
            "SynapseAI",
            "HMEM_SYNAPSEAI", 
            "HABANA_VISIBLE_DEVICES",
            validateSynapseAIDevice
        },
        {
            FI_HMEM_CUDA, 
            hmemCudaSupported_, 
            "CUDA",
            "HMEM_CUDA", 
            "CUDA_VISIBLE_DEVICES",
            validateCudaDevice
        },
        {
            FI_HMEM_ZE, 
            hmemZeSupported_, 
            "ZE",
            "HMEM_ZE", 
            "ZE_AFFINITY_MASK",
            validateZeDevice
        }
    };
    
    // 1 check explicit environment variable overrides first
    for (const auto& config : configs) {
        const char* explicit_env = getenv(config.explicit_env);
        if (isEnvTrue(explicit_env)) {
            if (!config.supported) {
                NIXL_ERROR << config.explicit_env << " set but " << config.name 
                          << " interface not supported by provider";
                return FI_HMEM_SYSTEM;
            }
            if (!config.validate(device_id)) {
                return FI_HMEM_SYSTEM;
            }
            NIXL_INFO << "Using " << config.name << " HMEM interface for device " 
                     << device_id << " (via " << config.explicit_env << ")";
            return config.iface;
        }
    }
    
    // 2 check implicit environment variables
    for (const auto& config : configs) {
        const char* implicit_env = getenv(config.implicit_env);
        if (implicit_env && config.supported) {
            if (!config.validate(device_id)) {
                if (config.iface == FI_HMEM_SYNAPSEAI) {
                    NIXL_ERROR << "  unset " << config.implicit_env << " to use host memory";
                }
                return FI_HMEM_SYSTEM;
            }
            NIXL_INFO << "Using " << config.name << " HMEM interface for device " 
                     << device_id << " (via " << config.implicit_env << ")";
            return config.iface;
        }
    }
    
    // 3 auto-select from supported interfaces
    NIXL_INFO << "No HMEM environment variables detected, auto-selecting interface for VRAM";
    for (const auto& config : configs) {
        if (config.supported && config.validate(device_id)) {
            NIXL_INFO << "Auto-selected " << config.name << " HMEM interface for device " << device_id;
            return config.iface;
        }
    }
    
    NIXL_WARN << "No HMEM interfaces supported. Falling back to host memory registration";
    return FI_HMEM_SYSTEM;
}

nixl_status_t nixlOfiEngine::registerDramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const {
    if (!domain_) {
        NIXL_ERROR << "Domain not initialized";
        return NIXL_ERR_BACKEND;
    }
    
    if (mem.addr == 0 || mem.len == 0) {
        NIXL_ERROR << "Invalid memory parameters: addr=" << mem.addr << " len=" << mem.len;
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // Check if this is actually device memory (HPU) that should use HMEM
    uint64_t device_id = 0;
    fi_hmem_iface iface = selectHmemInterface(mem, device_id);
    
    if (iface != FI_HMEM_SYSTEM) {
        if (iface == FI_HMEM_SYNAPSEAI) {
            // Use explicit SynapseAI registration to avoid hcclLookupDMABuff segfault
            NIXL_DEBUG << "DRAM_SEG memory detected as SynapseAI device memory, using explicit dmabuf registration";
            return registerSynapseAIMemoryExplicit(mem, ofi_meta);
        }
        // This is actually device memory, use HMEM registration
        NIXL_DEBUG << "DRAM_SEG memory detected as device memory, using HMEM registration";
        return registerVramMemory(mem, ofi_meta);
    }
    
    // Standard host DRAM registration
    uint64_t access_flags = getMemoryRegistrationAccessFlags(fi_);
    
    int ret = fi_mr_reg(domain_, reinterpret_cast<void*>(mem.addr), mem.len,
                       access_flags, 0, 0, 0, &ofi_meta->mr, nullptr);
    
    if (ret) {
        NIXL_ERROR << "fi_mr_reg failed for DRAM: " << fi_strerror(-ret);
        ofi_meta->mr = nullptr;
        return NIXL_ERR_BACKEND;
    }
    
    ofi_meta->desc = fi_mr_desc(ofi_meta->mr);
    
    uint64_t mr_key = fi_mr_key(ofi_meta->mr);
    NIXL_ERROR << "registerDramMemory: registered mr=" << ofi_meta->mr << " key=" << mr_key << " addr=0x" << std::hex << mem.addr << std::dec << " len=" << mem.len;
    if (!ofi_meta->desc) {
        NIXL_ERROR << "fi_mr_desc failed";
        fi_close(&ofi_meta->mr->fid);
        ofi_meta->mr = nullptr;
        return NIXL_ERR_BACKEND;
    }
    
    return NIXL_SUCCESS;
}


nixl_status_t nixlOfiEngine::registerSynapseAIMemoryExplicit(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const {
    // Try to get device info from the memory descriptor first
    // If mem.devId is a valid SynapseAI device handle, use it directly
    synDeviceId device_id = static_cast<synDeviceId>(mem.devId);
    synDeviceInfoV2 device_info;
    
    // Try to get device info directly using the device ID from memory descriptor
    NIXL_DEBUG << "Attempting to get device info for device ID: " << device_id;
    
    // thread-safe initialization of static handles
    std::lock_guard<std::mutex> lock(synapseai_init_mutex_);
    
    // load synapseAI library functions (shared across instances)
    if (!synapseai_handle_) {
        synapseai_handle_ = dlopen("libSynapse.so", RTLD_NOW);
        if (!synapseai_handle_) {
            NIXL_ERROR << "failed to dlopen libSynapse.so: " << dlerror();
            return NIXL_ERR_BACKEND;
        }
        
        synapseai_ops_.synDeviceGetInfoV2 = 
            (synStatus (*)(const synDeviceId, synDeviceInfoV2 *))dlsym(synapseai_handle_, "synDeviceGetInfoV2");
        if (!synapseai_ops_.synDeviceGetInfoV2) {
            NIXL_ERROR << "failed to find synDeviceGetInfoV2: " << dlerror();
            return NIXL_ERR_BACKEND;
        }
    }
    
    if (!hlthunk_handle_) {
        hlthunk_handle_ = dlopen("libhl-thunk.so", RTLD_NOW);
        if (!hlthunk_handle_) {
            NIXL_ERROR << "failed to dlopen libhl-thunk.so: " << dlerror();
            return NIXL_ERR_BACKEND;
        }
        
        synapseai_ops_.hlthunk_device_mapped_memory_export_dmabuf_fd = 
            (int (*)(int, uint64_t, uint64_t, uint64_t, uint32_t))dlsym(hlthunk_handle_, "hlthunk_device_mapped_memory_export_dmabuf_fd");
        if (!synapseai_ops_.hlthunk_device_mapped_memory_export_dmabuf_fd) {
            NIXL_ERROR << "failed to find hlthunk_device_mapped_memory_export_dmabuf_fd: " << dlerror();
            return NIXL_ERR_BACKEND;
        }
    }
    
    // Check if device is available first
    if (synapseai_ops_.synDeviceGetInfoV2(device_id, &device_info) != synSuccess) {
        NIXL_INFO << "SynapseAI device " << device_id << " not available, falling back to DRAM registration";
        return registerDramMemory(mem, ofi_meta);
    }
    
    NIXL_INFO << "Using existing SynapseAI device (PyTorch initialized) ID: " << device_id;

    // Calculate aligned buffer size
    const size_t ACCEL_PAGE_SIZE = 4096;
    size_t modi_memlen = mem.len;
    
    // Check if memory is within device range
    uint64_t hbm_base = device_info.globalHbmBaseAddress;
    uint64_t hbm_size = device_info.dramSize;
    
    NIXL_DEBUG << "Memory validation: addr=0x" << std::hex << mem.addr 
              << " HBM_base=0x" << hbm_base 
              << " HBM_size=0x" << hbm_size << std::dec;
    
    if (mem.addr < hbm_base || mem.addr >= (hbm_base + hbm_size)) {
        NIXL_ERROR << "Memory address 0x" << std::hex << mem.addr 
                  << " is not within HPU device memory range [0x" << hbm_base 
                  << " - 0x" << (hbm_base + hbm_size) << "]";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // Align device offset to suit page size
    uint64_t device_offset = mem.addr - hbm_base;
    uint64_t modi_mem_addr = mem.addr;
    if (mem.addr % ACCEL_PAGE_SIZE) {
        modi_mem_addr = (mem.addr / ACCEL_PAGE_SIZE) * ACCEL_PAGE_SIZE;
        device_offset -= mem.addr - modi_mem_addr;
        modi_memlen += ACCEL_PAGE_SIZE;
    }
    modi_memlen = (modi_memlen + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

    NIXL_INFO << "Exporting dmabuf: fd=" << device_info.fd
              << " base=0x" << std::hex << hbm_base 
              << " size=" << std::dec << modi_memlen
              << " tensor data ptr=0x" << std::hex << mem.addr
              << " modified tensor data ptr=0x" << std::hex << modi_mem_addr
              << " offset=0x" << std::hex << device_offset;
    
    // Get dmabuf fd
    int dmabuf_fd = synapseai_ops_.hlthunk_device_mapped_memory_export_dmabuf_fd(
        device_info.fd,
        hbm_base,
        modi_memlen,
        device_offset,
        (O_RDWR | O_CLOEXEC)
    );
    
    if (dmabuf_fd < 0) {
        NIXL_ERROR << "hlthunk_device_mapped_memory_export_dmabuf_fd failed: " << strerror(-dmabuf_fd);
        NIXL_ERROR << "  device_fd=" << device_info.fd;
        NIXL_ERROR << "  base_addr=0x" << std::hex << hbm_base;
        NIXL_ERROR << "  size=" << std::dec << modi_memlen;
        NIXL_ERROR << "  offset=0x" << std::hex << device_offset;
        return NIXL_ERR_BACKEND;
    }
    
    NIXL_DEBUG << "Got dmabuf_fd: " << dmabuf_fd << " for device memory addr: 0x" 
              << std::hex << mem.addr << " size: " << std::dec << mem.len;
    
    // set up dmabuf structure - fix page alignment issue
    // kernel exported page-aligned region, but we register exact buffer
    struct fi_mr_dmabuf dmabuf = {};
    dmabuf.fd = dmabuf_fd;
    dmabuf.offset = 0;                                   // kernel handled offset
    dmabuf.len = modi_memlen;                                // page-aligned buffer size
    dmabuf.base_addr = reinterpret_cast<void*>(modi_mem_addr); // page-aligned buffer start
    dmabuf.offset = 0;                                   // kernel handled offset
    
    // Set up memory registration attributes
    struct fi_mr_attr mr_attr = {};
    mr_attr.dmabuf = &dmabuf;
    mr_attr.iov_count = 1;
    mr_attr.access = getMemoryRegistrationAccessFlags(fi_);
    mr_attr.iface = FI_HMEM_SYNAPSEAI;
    mr_attr.device.synapseai = static_cast<uint32_t>(device_id);
    
    NIXL_DEBUG << "Registering SynapseAI memory with explicit dmabuf fd: " << dmabuf_fd;
    
    // register memory with explicit dmabuf
    int ret = fi_mr_regattr(domain_, &mr_attr, FI_MR_DMABUF, &ofi_meta->mr);
    
    // cleanup fd after registration
    close(dmabuf_fd);
    
    if (ret) {
        NIXL_ERROR << "memory registration failed: " << fi_strerror(-ret);
        ofi_meta->mr = nullptr;
        return NIXL_ERR_BACKEND;
    }
    
    // set descriptor
    ofi_meta->desc = fi_mr_desc(ofi_meta->mr);
    if (!ofi_meta->desc) {
        NIXL_ERROR << "fi_mr_desc failed";
        fi_close(&ofi_meta->mr->fid);
        ofi_meta->mr = nullptr;
        return NIXL_ERR_BACKEND;
    }
    
    NIXL_INFO << "successfully registered SynapseAI memory via dmabuf";
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::registerVramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const {
    if (!domain_) {
        NIXL_ERROR << "Domain not initialized";
        return NIXL_ERR_BACKEND;
    }
    
    if (mem.addr == 0 || mem.len == 0) {
        NIXL_ERROR << "Invalid memory parameters: addr=" << mem.addr << " len=" << mem.len;
        return NIXL_ERR_INVALID_PARAM;
    }
    
    struct fi_mr_attr mr_attr = {};
    struct iovec iov = {};
    
    iov.iov_base = reinterpret_cast<void*>(mem.addr);
    iov.iov_len = mem.len;
    
    mr_attr.mr_iov = &iov;
    mr_attr.iov_count = 1;
    mr_attr.access = getMemoryRegistrationAccessFlags(fi_);
    
    uint64_t device_id = 0;
    mr_attr.iface = selectHmemInterface(mem, device_id);
    
    if (mr_attr.iface == FI_HMEM_SYSTEM) {
        NIXL_WARN << "VRAM requested but HMEM interface unavailable - falling back to system memory registration";
        return registerDramMemory(mem, ofi_meta);
    }

    // Use explicit dmabuf registration for SynapseAI to avoid hcclLookupDMABuff segfault
    if (mr_attr.iface == FI_HMEM_SYNAPSEAI) {
        NIXL_DEBUG << "Using explicit SynapseAI dmabuf registration (fabtests approach)";
        return registerSynapseAIMemoryExplicit(mem, ofi_meta);
    }
    
    if (device_id >= UINT32_MAX) {
        NIXL_ERROR << "Invalid device ID: " << device_id;
        return NIXL_ERR_INVALID_PARAM;
    }
    
    switch (mr_attr.iface) {
        case FI_HMEM_CUDA:
            mr_attr.device.cuda = static_cast<uint32_t>(device_id);
            break;
        case FI_HMEM_ZE:
            mr_attr.device.ze = static_cast<uint32_t>(device_id);
            break;
        case FI_HMEM_SYNAPSEAI:
            mr_attr.device.synapseai = static_cast<uint32_t>(device_id);
            break;
        default:
            mr_attr.device.reserved = 0;
            break;
    }
    
    uint64_t reg_flags = 0;
    if (fi_ && fi_->domain_attr && fi_->domain_attr->mr_mode) {
        if (fi_->domain_attr->mr_mode & FI_MR_HMEM) {
            reg_flags |= FI_HMEM_DEVICE_ONLY;
        }
        NIXL_DEBUG << "Provider MR mode: 0x" << std::hex << fi_->domain_attr->mr_mode 
                  << " using reg_flags: 0x" << reg_flags;
    }
    
    NIXL_DEBUG << "Registering VRAM memory with interface " << mr_attr.iface 
              << " access 0x" << std::hex << mr_attr.access 
              << " flags 0x" << reg_flags;
              
    int ret = fi_mr_regattr(domain_, &mr_attr, reg_flags, &ofi_meta->mr);
    
    if (ret) {
        ofi_meta->mr = nullptr;
        
        if (mr_attr.iface == FI_HMEM_SYNAPSEAI) {
            NIXL_ERROR << "SynapseAI device memory registration failed: " << fi_strerror(-ret);
            
            // provide specific guidance based on error code
            switch (-ret) {
                case EBUSY:
                    NIXL_ERROR << "  Device is busy - another process may be using it";
                    NIXL_ERROR << "  Try: fuser -v /dev/accel/accel*";
                    break;
                case ENOMEM:
                    NIXL_ERROR << "  Device memory exhausted - try smaller allocation";
                    NIXL_ERROR << "  Check device memory usage with habana monitoring tools";
                    break;
                case ENODEV:
                    NIXL_ERROR << "  Device not available - check driver status";
                    NIXL_ERROR << "  Try: systemctl status habana-driver";
                    break;
                case EFAULT:
                    NIXL_ERROR << "  Invalid memory address - check DMA-BUF mapping";
                    break;
                default:
                    NIXL_ERROR << "  SynapseAI device is busy or inaccessible.";
                    break;
            }
            NIXL_ERROR << "  To use host memory: unset HABANA_VISIBLE_DEVICES";
        } else {
            NIXL_ERROR << "fi_mr_regattr failed: " << fi_strerror(-ret);
        }
        return NIXL_ERR_BACKEND;
    }
    
    // Set descriptor for successful registration
    ofi_meta->desc = fi_mr_desc(ofi_meta->mr);
    
    NIXL_DEBUG << "Memory registration successful:";
    NIXL_DEBUG << "  addr=0x" << std::hex << mem.addr << " len=" << std::dec << mem.len;
    NIXL_DEBUG << "  mr=" << ofi_meta->mr << " key=" << fi_mr_key(ofi_meta->mr);
    NIXL_DEBUG << "  desc=" << ofi_meta->desc;
    
    return NIXL_SUCCESS;
}

bool nixlOfiEngine::isConnectionEstablished(const std::string& remote_agent) const {
    std::lock_guard<std::mutex> lock(epLock_);
    
    if (isConnectionless_) {
        bool found = avAddrs_.find(remote_agent) != avAddrs_.end();
        NIXL_DEBUG << "connectionless address check for " << remote_agent << ": " 
                  << (found ? "found" : "not found") << " (total AV entries: " << avAddrs_.size() << ")";
        return found;
    } else {
        bool found = connectedEps_.find(remote_agent) != connectedEps_.end();
        NIXL_DEBUG << "connection-oriented check for " << remote_agent << ": " 
                  << (found ? "connected" : "not connected") << " (total connections: " << connectedEps_.size() << ")";
        return found;
    }
}

nixl_status_t nixlOfiEngine::validateTransferParams(const nixlMetaDesc& local_desc, 
                                                     const nixlMetaDesc& remote_desc) const {
    // check metadata pointers
    if (!local_desc.metadataP || !remote_desc.metadataP) {
        NIXL_ERROR << "null metadata pointers";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // check addresses and lengths
    if (local_desc.addr == 0 || local_desc.len == 0 || 
        remote_desc.addr == 0 || remote_desc.len == 0) {
        NIXL_ERROR << "invalid address or length";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // check length match
    if (local_desc.len != remote_desc.len) {
        NIXL_ERROR << "length mismatch: local=" << local_desc.len 
                  << " remote=" << remote_desc.len;
        return NIXL_ERR_INVALID_PARAM;
    }
    
    // validate local memory registration
    nixlOfiMetadata *local_meta = static_cast<nixlOfiMetadata*>(local_desc.metadataP);
    if (!local_meta->mr) {
        NIXL_ERROR << "invalid local memory registration";
        return NIXL_ERR_INVALID_PARAM;
    }
    
    return NIXL_SUCCESS;
}

uint64_t nixlOfiEngine::getRemoteKey(nixlOfiMetadata* remote_meta) const {
    if (remote_meta->mr) {
        return fi_mr_key(remote_meta->mr);
    } else {
        // for remote metadata, key is stored in desc field
        uintptr_t desc_as_ptr = reinterpret_cast<uintptr_t>(remote_meta->desc);
        return static_cast<uint64_t>(desc_as_ptr);
    }
}

nixl_status_t nixlOfiEngine::handleCQError(fid_cq* cq, int error_ret) const {
    if (error_ret == -FI_EAGAIN) {
        return NIXL_IN_PROG;
    }
    
    struct fi_cq_err_entry err_entry;
    int err_ret = fi_cq_readerr(cq, &err_entry, 0);
    if (err_ret > 0) {
        NIXL_ERROR << "CQ error: " << fi_strerror(err_entry.err) << " (" << err_entry.err << ")";
        // context will be cleaned up automatically by unique_ptr when request is destroyed
        if (err_entry.op_context) {
            NIXL_ERROR << "error context: " << err_entry.op_context;
        }
    } else {
        NIXL_ERROR << "fi_cq_read failed: " << fi_strerror(-error_ret);
    }
    return NIXL_ERR_BACKEND;
}

