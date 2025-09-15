/*
 * Copyright (c) 2025 Intel Corporation. All rights reserved.
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
#include <arpa/inet.h>
#include <netinet/in.h>

// static member definitions
struct fi_info *nixlOfiUtils::hints = nullptr;
struct fi_info *nixlOfiUtils::fi = nullptr;
struct fid_fabric *nixlOfiUtils::fabric = nullptr;
struct fid_domain *nixlOfiUtils::domain = nullptr;
struct fid_ep *nixlOfiUtils::ep = nullptr;
struct fid_cq *nixlOfiUtils::txcq = nullptr;
struct fid_cq *nixlOfiUtils::rxcq = nullptr;
struct fid_av *nixlOfiUtils::av = nullptr;
bool nixlOfiUtils::fabric_initialized = false;

nixl_status_t nixlOfiUtils::initOfi() {
    NIXL_INFO << "initializing ofi utilities";
    return NIXL_SUCCESS;
}

void nixlOfiUtils::cleanupOfi() {
    NIXL_INFO << "cleaning up ofi utilities";
    cleanupFabric();
}

nixl_status_t nixlOfiUtils::setupFabric(const nixl_b_params_t& params) {
    if (fabric_initialized) {
        NIXL_INFO << "fabric already initialized";
        return NIXL_SUCCESS;
    }

    NIXL_INFO << "setting up ofi fabric";
    int ret;

    // configure hints
    hints = fi_allocinfo();
    if (!hints) {
        NIXL_ERROR << "failed to allocate fi_info";
        return NIXL_ERR_BACKEND;
    }

    hints->caps = FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    hints->mode = FI_CONTEXT;
    hints->addr_format = FI_FORMAT_UNSPEC;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->threading = FI_THREAD_DOMAIN;
    hints->domain_attr->control_progress = FI_PROGRESS_UNSPEC;
    hints->domain_attr->data_progress = FI_PROGRESS_UNSPEC;
    hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
    hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_RAW | FI_MR_VIRT_ADDR |
                                  FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT;

    // set provider from params with default
    std::string provider = get_param_string(params, "ofi_provider", "verbs;ofi_rxm");
    if (!provider.empty()) {
        hints->fabric_attr->prov_name = strdup(provider.c_str());
        NIXL_INFO << "requesting provider: " << provider;
    } else {
        NIXL_INFO << "no provider specified, using libfabric default";
    }

    // get fabric info
    uint64_t flags = FI_SOURCE;
    ret = fi_getinfo(FI_VERSION(1, 20), nullptr, nullptr, flags, hints, &fi);
    if (ret) {
        if (hints->fabric_attr->prov_name) {
            NIXL_WARN << "requested provider '" << hints->fabric_attr->prov_name
                      << "' not found, trying default: " << fi_strerror(-ret);
            free(hints->fabric_attr->prov_name);
            hints->fabric_attr->prov_name = nullptr;
            ret = fi_getinfo(FI_VERSION(1, 20), nullptr, nullptr, flags, hints, &fi);
        }
        if (ret) {
            NIXL_ERROR << "fi_getinfo failed: " << fi_strerror(-ret);
            fi_freeinfo(hints);
            hints = nullptr;
            return NIXL_ERR_BACKEND;
        }
    }

    NIXL_INFO << "using provider: " << fi->fabric_attr->prov_name;

    // create fabric
    ret = fi_fabric(fi->fabric_attr, &fabric, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_fabric failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // create domain
    ret = fi_domain(fabric, fi, &domain, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_domain failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // create endpoint
    ret = fi_endpoint(domain, fi, &ep, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_endpoint failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // create completion queues
    struct fi_cq_attr cq_attr = {0};

    // get tx cq size from params
    cq_attr.size = get_param_int(params, "tx_cq_size", 1024);

    ret = fi_cq_open(domain, &cq_attr, &txcq, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_cq_open for tx failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // get rx cq size from params
    cq_attr.size = get_param_int(params, "rx_cq_size", 1024);

    ret = fi_cq_open(domain, &cq_attr, &rxcq, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_cq_open for rx failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // create address vector (initialize with default constructor then set fields explicitly)
    struct fi_av_attr av_attr = {};
    av_attr.type = FI_AV_TABLE; // use table-based address vector for simplicity
    av_attr.count = 1;          // initial capacity (libfabric may resize internally)
    ret = fi_av_open(domain, &av_attr, &av, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_av_open failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // bind resources to endpoint
    ret = fi_ep_bind(ep, &av->fid, 0);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind av failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(ep, &txcq->fid, FI_TRANSMIT);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind txcq failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(ep, &rxcq->fid, FI_RECV);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind rxcq failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // enable endpoint
    ret = fi_enable(ep);
    if (ret) {
        NIXL_ERROR << "fi_enable failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    fabric_initialized = true;
    NIXL_INFO << "ofi fabric setup complete";
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiUtils::cleanupFabric() {
    if (!fabric_initialized) {
        return NIXL_SUCCESS;
    }

    NIXL_INFO << "cleaning up ofi fabric";
    int ret;

    // close resources in reverse order
    if (ep) {
        ret = fi_close(&ep->fid);
        if (ret) NIXL_WARN << "error closing endpoint: " << fi_strerror(-ret);
        ep = nullptr;
    }

    if (av) {
        ret = fi_close(&av->fid);
        if (ret) NIXL_WARN << "error closing av: " << fi_strerror(-ret);
        av = nullptr;
    }

    if (rxcq) {
        ret = fi_close(&rxcq->fid);
        if (ret) NIXL_WARN << "error closing rxcq: " << fi_strerror(-ret);
        rxcq = nullptr;
    }

    if (txcq) {
        ret = fi_close(&txcq->fid);
        if (ret) NIXL_WARN << "error closing txcq: " << fi_strerror(-ret);
        txcq = nullptr;
    }

    if (domain) {
        ret = fi_close(&domain->fid);
        if (ret) NIXL_WARN << "error closing domain: " << fi_strerror(-ret);
        domain = nullptr;
    }

    if (fabric) {
        ret = fi_close(&fabric->fid);
        if (ret) NIXL_WARN << "error closing fabric: " << fi_strerror(-ret);
        fabric = nullptr;
    }

    if (fi) {
        fi_freeinfo(fi);
        fi = nullptr;
    }

    if (hints) {
        fi_freeinfo(hints);
        hints = nullptr;
    }

    fabric_initialized = false;
    NIXL_INFO << "ofi fabric cleanup complete";
    return NIXL_SUCCESS;
}

nixl_status_t ofi_status_to_nixl(int ofi_status) {
    // placeholder status conversion
    switch (ofi_status) {
    case 0: // fi_success equivalent
        return NIXL_SUCCESS;
    case -11: // -eagain equivalent
        return NIXL_IN_PROG;
    default:
        return NIXL_ERR_BACKEND;
    }
}

nixl_b_params_t get_ofi_backend_common_options() {
    // common ofi backend options
    nixl_b_params_t params = {
        {"ofi_provider", "verbs;ofi_rxm"},  // default to verbs with rxm utility provider
        {"ofi_domain", ""},                 // domain selection
        {"num_workers", "1"},               // number of worker threads
        {"tx_cq_size", "1024"},             // transmit completion queue size
        {"rx_cq_size", "1024"},             // receive completion queue size
        {"retry_count", "1000"},            // max retries for -FI_EAGAIN operations
        {"retry_delay_us", "1"}             // delay in microseconds between retries
    };
    return params;
}

int get_param_int(const nixl_b_params_t& params, const std::string& key, int default_value) {
    auto it = params.find(key);
    if (it != params.end() && !it->second.empty()) {
        try {
            return std::stoi(it->second);
        } catch (const std::exception&) {
            NIXL_WARN << "invalid parameter value for " << key << ": " << it->second
                      << ", using default " << default_value;
        }
    }
    return default_value;
}

std::string get_param_string(const nixl_b_params_t& params, const std::string& key, const std::string& default_value) {
    auto it = params.find(key);
    if (it != params.end() && !it->second.empty()) {
        return it->second;
    }
    return default_value;
}

std::string addr_to_string(const void* addr_data, size_t addr_len) {
    if (!addr_data || addr_len < sizeof(struct sockaddr)) {
        return "invalid";
    }

    const struct sockaddr* sa = reinterpret_cast<const struct sockaddr*>(addr_data);
    char addr_str[INET6_ADDRSTRLEN];

    if (sa->sa_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
        if (inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str))) {
            return std::string(addr_str) + ":" + std::to_string(ntohs(sin->sin_port));
        }
    } else if (sa->sa_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6* sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str))) {
            return "[" + std::string(addr_str) + "]:" + std::to_string(ntohs(sin6->sin6_port));
        }
    }

    return "unknown_family_" + std::to_string(sa->sa_family);
}

void drive_manual_progress() {
    if (!nixlOfiUtils::fabric_initialized) {
        return;
    }

    // Drive progress on all completion queues to advance RMA operations
    // This is CRITICAL for FI_PROGRESS_MANUAL providers like verbs;ofi_rxm

    if (nixlOfiUtils::txcq) {
        // Drive TX progress - RMA operations post to TX queue
        struct fi_cq_entry comp[4];
        int ret = fi_cq_read(nixlOfiUtils::txcq, comp, 4);
        if (ret > 0) {
            NIXL_DEBUG << "[MANUAL_PROGRESS] processed " << ret << " TX completions";
        } else if (ret == -FI_EAVAIL) {
            // Handle CQ errors but continue driving progress
            struct fi_cq_err_entry err;
            fi_cq_readerr(nixlOfiUtils::txcq, &err, 0);
            NIXL_DEBUG << "[MANUAL_PROGRESS] TX CQ error: " << fi_strerror(err.err);
        }
    }

    if (nixlOfiUtils::rxcq) {
        // Drive RX progress - may be needed for some providers
        struct fi_cq_entry comp[4];
        int ret = fi_cq_read(nixlOfiUtils::rxcq, comp, 4);
        if (ret > 0) {
            NIXL_DEBUG << "[MANUAL_PROGRESS] processed " << ret << " RX completions";
        } else if (ret == -FI_EAVAIL) {
            struct fi_cq_err_entry err;
            fi_cq_readerr(nixlOfiUtils::rxcq, &err, 0);
            NIXL_DEBUG << "[MANUAL_PROGRESS] RX CQ error: " << fi_strerror(err.err);
        }
    }
}
