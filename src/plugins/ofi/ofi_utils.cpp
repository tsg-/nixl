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
#include <algorithm>
#include <string>
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

    hints->caps = FI_MSG | FI_RMA | FI_READ | FI_REMOTE_READ; 
    hints->mode = 0;
    hints->addr_format = FI_FORMAT_UNSPEC;
    hints->tx_attr->tclass = 0x203;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    hints->domain_attr->control_progress = FI_PROGRESS_UNSPEC;
    hints->domain_attr->data_progress = FI_PROGRESS_UNSPEC;
    hints->domain_attr->resource_mgmt = FI_RM_ENABLED;

    hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;

    // set tx/rx attributes to enable completions as per verbs config
    // hints->tx_attr->op_flags = FI_COMPLETION;
    // hints->rx_attr->op_flags = FI_COMPLETION;

    // set provider from params with default
    std::string provider = get_param_string(params, "ofi_provider", "verbs");
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
                      << "' not found: " << fi_strerror(-ret) << ". trying defaults";
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

    // validate that provider meets our requirements
    validate_provider_capabilities(hints, fi);

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
    cq_attr.format = FI_CQ_FORMAT_CONTEXT; // standard completion format
    cq_attr.wait_obj = FI_WAIT_NONE;       // no wait object, manual polling

    // get tx cq size from params - use larger default for better buffering
    cq_attr.size = get_param_int(params, "tx_cq_size", 4096);

    ret = fi_cq_open(domain, &cq_attr, &txcq, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_cq_open for tx failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // get rx cq size from params - use larger default for better buffering
    cq_attr.size = get_param_int(params, "rx_cq_size", 4096);

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

    // Bind TX CQ - start with basic FI_TRANSMIT, RMA completions will appear here
    ret = fi_ep_bind(ep, &txcq->fid, FI_TRANSMIT);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind txcq failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    NIXL_INFO << "TX CQ bound with: FI_TRANSMIT (RMA completions will appear on this CQ)";

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

    // for verbs;ofi_rxm provider, post some receive buffers to avoid -FI_EAGAIN
    // this is required for proper RMA operation with RXM utility provider
    if (fi && fi->fabric_attr && fi->fabric_attr->prov_name) {
        std::string prov_name(fi->fabric_attr->prov_name);
        if (prov_name.find("rxm") != std::string::npos) {
            NIXL_INFO << "detected RXM provider, posting initial receive buffers";

            // allocate small receive buffers
            const size_t recv_buf_size = 64; // small control messages
            const int num_recv_bufs = 16;

            for (int i = 0; i < num_recv_bufs; i++) {
                void* recv_buf = malloc(recv_buf_size);
                if (recv_buf) {
                    ret = fi_recv(ep, recv_buf, recv_buf_size, nullptr, FI_ADDR_UNSPEC, recv_buf);
                    if (ret && ret != -FI_EAGAIN) {
                        NIXL_WARN << "fi_recv failed: " << fi_strerror(-ret);
                        free(recv_buf);
                        break;
                    }
                }
            }

            // drive progress to ensure receive buffers are processed
            for (int i = 0; i < 100; i++) {
                struct fi_cq_entry comp[4];
                fi_cq_read(rxcq, comp, 4);
                fi_cq_read(txcq, comp, 4);
            }
        }
    }

    fabric_initialized = true;
    NIXL_INFO << "ofi fabric setup complete";

    // Log progress model information
    NIXL_INFO << "LibFabric Progress Model:";
    NIXL_INFO << "  Data Progress: " << (fi->domain_attr->data_progress == FI_PROGRESS_MANUAL ? "MANUAL" :
                                        fi->domain_attr->data_progress == FI_PROGRESS_AUTO ? "AUTO" : "UNSPEC");
    NIXL_INFO << "  Control Progress: " << (fi->domain_attr->control_progress == FI_PROGRESS_MANUAL ? "MANUAL" :
                                           fi->domain_attr->control_progress == FI_PROGRESS_AUTO ? "AUTO" : "UNSPEC");

    if (fi->domain_attr->data_progress == FI_PROGRESS_MANUAL) {
        NIXL_WARN << "*** MANUAL PROGRESS REQUIRED for data operations (including RMA) ***";
    }

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
        {"ofi_provider", "verbs;ofi_rxm"},  // use what's available and fix the issues
        {"ofi_domain", ""},                 // domain selection
        {"num_workers", "1"},               // number of worker threads
        {"tx_cq_size", "4096"},             // transmit completion queue size
        {"rx_cq_size", "4096"},             // receive completion queue size
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

bool get_param_bool(const nixl_b_params_t& params, const std::string& key, bool default_value) {
    auto it = params.find(key);
    if (it != params.end() && !it->second.empty()) {
        std::string value = it->second;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return (value == "true" || value == "1" || value == "yes" || value == "on");
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

int ofi_progress(struct fid_cq *cq) {
    if (!nixlOfiUtils::fabric_initialized) {
        return 0;
    }

    struct fi_cq_err_entry comp;
    int ret = fi_cq_read(cq, &comp, 1);

    if (ret >= 0 || ret == -FI_EAGAIN) {
        return 0;
    }

    if (ret == -FI_EAVAIL) {
        struct fi_cq_err_entry err;
        fi_cq_readerr(cq, &err, 0);
        NIXL_ERROR << "CQ error: " << fi_strerror(err.err);
        return -1;
    }

    NIXL_ERROR << "fi_cq_read failed: " << fi_strerror(-ret);
    return -1;
}

void simple_progress() {
    fi_cq_read(nixlOfiUtils::rxcq, nullptr, 0);
    fi_cq_read(nixlOfiUtils::txcq, nullptr, 0);
}

void drive_manual_progress() {
    if (!nixlOfiUtils::fabric_initialized) {
        return;
    }

#if 0
    // drive completion queues and connection management progress
    if (nixlOfiUtils::txcq) {
        struct fi_cq_entry comp[16];
        int ret = fi_cq_read(nixlOfiUtils::txcq, comp, 16);
        if (ret > 0) {
            NIXL_INFO << "[MANUAL_PROGRESS] processed " << ret << " TX completions";
        } else if (ret == -FI_EAVAIL) {
            struct fi_cq_err_entry err;
            fi_cq_readerr(nixlOfiUtils::txcq, &err, 0);
            NIXL_INFO << "[MANUAL_PROGRESS] TX CQ error: " << fi_strerror(err.err);
        }
    }

    if (nixlOfiUtils::rxcq) {
        struct fi_cq_entry comp[16];
        int ret = fi_cq_read(nixlOfiUtils::rxcq, comp, 16);
        if (ret > 0) {
            NIXL_INFO << "[MANUAL_PROGRESS] processed " << ret << " RX completions";
        } else if (ret == -FI_EAVAIL) {
            struct fi_cq_err_entry err;
            fi_cq_readerr(nixlOfiUtils::rxcq, &err, 0);
            NIXL_INFO << "[MANUAL_PROGRESS] RX CQ error: " << fi_strerror(err.err);
        }
    }
#endif

    // drive connection management progress for rxm
    if (nixlOfiUtils::txcq) {
        ofi_progress(nixlOfiUtils::txcq);
    }
    if (nixlOfiUtils::rxcq) {
        ofi_progress(nixlOfiUtils::rxcq);
    }
}

void validate_provider_capabilities(struct fi_info* hints, struct fi_info* result) {
    if (!hints || !result) {
        NIXL_ERROR << "Invalid arguments to validate_provider_capabilities";
        return;
    }

#define NIXL_CHECK_PROVIDER_ATTR(condition, attr_name, req_val, res_val, to_str_type) \
    NIXL_ASSERT_ALWAYS(condition) \
        << "Provider does not support requested " << attr_name << ". " \
        << "Requested: " << fi_tostr(&(req_val), to_str_type) \
        << ", Got: " << fi_tostr(&(res_val), to_str_type)

    // capabilities
    NIXL_CHECK_PROVIDER_ATTR((result->caps & hints->caps) == hints->caps,
        "capabilities", hints->caps, result->caps, FI_TYPE_CAPS);

    // modes (only check if we specified requirements)
    if (hints->mode != 0) {
        NIXL_CHECK_PROVIDER_ATTR((result->mode & hints->mode) == hints->mode,
            "modes", hints->mode, result->mode, FI_TYPE_MODE);
    }

    // endpoint type
    if (hints->ep_attr && result->ep_attr) {
        NIXL_CHECK_PROVIDER_ATTR(result->ep_attr->type == hints->ep_attr->type,
            "endpoint type", hints->ep_attr->type, result->ep_attr->type, FI_TYPE_EP_TYPE);
    }

    // domain attributes
    if (hints->domain_attr && result->domain_attr) {
        // memory registration mode
        if (hints->domain_attr->mr_mode != 0) {
            NIXL_CHECK_PROVIDER_ATTR((result->domain_attr->mr_mode & hints->domain_attr->mr_mode) == hints->domain_attr->mr_mode,
                "mr_mode", hints->domain_attr->mr_mode, result->domain_attr->mr_mode, FI_TYPE_MR_MODE);
        }

        // threading model
        if (hints->domain_attr->threading != FI_THREAD_UNSPEC) {
            NIXL_CHECK_PROVIDER_ATTR(result->domain_attr->threading == hints->domain_attr->threading,
                "threading model", hints->domain_attr->threading, result->domain_attr->threading, FI_TYPE_THREADING);
        }

        // resource management model
        if (hints->domain_attr->resource_mgmt != FI_RM_UNSPEC) {
            NIXL_ASSERT_ALWAYS(result->domain_attr->resource_mgmt == hints->domain_attr->resource_mgmt)
                << "Provider does not support requested resource_mgmt. "
                << "Requested: " << (hints->domain_attr->resource_mgmt == FI_RM_ENABLED ? "FI_RM_ENABLED" : "OTHER")
                << ", Got: " << (result->domain_attr->resource_mgmt == FI_RM_ENABLED ? "FI_RM_ENABLED" : "OTHER");
        }
    }

    // address format
    if (hints->addr_format != FI_FORMAT_UNSPEC) {
        NIXL_CHECK_PROVIDER_ATTR(result->addr_format == hints->addr_format,
            "address format", hints->addr_format, result->addr_format, FI_TYPE_ADDR_FORMAT);
    }

    // tx attributes
    if (hints->tx_attr && result->tx_attr && hints->tx_attr->op_flags != 0) {
        NIXL_CHECK_PROVIDER_ATTR((result->tx_attr->op_flags & hints->tx_attr->op_flags) == hints->tx_attr->op_flags,
            "tx_attr->op_flags", hints->tx_attr->op_flags, result->tx_attr->op_flags, FI_TYPE_OP_FLAGS);
    }

    // rx attributes
    if (hints->rx_attr && result->rx_attr && hints->rx_attr->op_flags != 0) {
        NIXL_CHECK_PROVIDER_ATTR((result->rx_attr->op_flags & hints->rx_attr->op_flags) == hints->rx_attr->op_flags,
            "rx_attr->op_flags", hints->rx_attr->op_flags, result->rx_attr->op_flags, FI_TYPE_OP_FLAGS);
    }

#undef NIXL_CHECK_PROVIDER_ATTR

    NIXL_INFO << "provider capability validation passed";
    NIXL_INFO << "provider modes: " << fi_tostr(&(result->mode), FI_TYPE_MODE);
}
