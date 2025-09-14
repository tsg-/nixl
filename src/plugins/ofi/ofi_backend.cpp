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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <chrono>
#include <thread>

// internal helpers to decode capability and mode bitmasks for easier debug
namespace {
std::string join(const std::vector<std::string>& parts, const char* delim="|") {
    if (parts.empty()) return "";
    std::ostringstream oss;
    for (size_t i=0;i<parts.size();++i) {
        if (i) oss << delim;
        oss << parts[i];
    }
    return oss.str();
}

std::string decodeCaps(uint64_t caps) {
    std::vector<std::string> names;
    if (caps & FI_MSG) names.push_back("MSG");
    if (caps & FI_RMA) names.push_back("RMA");
    if (caps & FI_READ) names.push_back("READ");
    if (caps & FI_WRITE) names.push_back("WRITE");
    if (caps & FI_REMOTE_READ) names.push_back("REMOTE_READ");
    if (caps & FI_REMOTE_WRITE) names.push_back("REMOTE_WRITE");
    if (caps & FI_ATOMIC) names.push_back("ATOMIC");
    if (caps & FI_TAGGED) names.push_back("TAGGED");
    return join(names);
}

std::string decodeMrMode(uint64_t mr_mode) {
    std::vector<std::string> names;
    if (mr_mode & FI_MR_LOCAL) names.push_back("LOCAL");
#ifdef FI_MR_RAW
    if (mr_mode & FI_MR_RAW) names.push_back("RAW");
#endif
    if (mr_mode & FI_MR_VIRT_ADDR) names.push_back("VIRT_ADDR");
    if (mr_mode & FI_MR_ALLOCATED) names.push_back("ALLOCATED");
    if (mr_mode & FI_MR_PROV_KEY) names.push_back("PROV_KEY");
#ifdef FI_MR_ENDPOINT
    if (mr_mode & FI_MR_ENDPOINT) names.push_back("ENDPOINT");
#endif
    if (mr_mode & FI_MR_BASIC) names.push_back("BASIC");
    return join(names);
}
}

nixlOfiEngine::nixlOfiEngine(const nixlBackendInitParams* init_params) :
    nixlBackendEngine(init_params),
    fabric_(nullptr),
    domain_(nullptr),
    ep_(nullptr),
    txcq_(nullptr),
    rxcq_(nullptr),
    av_(nullptr),
    fi_(nullptr),
    ep_ready_(false)
{
    struct fi_info *hints = fi_allocinfo();
    if (!hints) {
        this->initErr = true;
        return;
    }
    std::cerr << "OFI_DEBUG: after fi_allocinfo\n";

    // Base hint configuration: start minimally (fabtests parity) and expand lazily when operations require.
    // We request only MSG + RMA + READ capabilities plus REMOTE_READ. WRITE/REMOTE_WRITE are added on-demand
    // (or via env) to reduce provider resource allocation pressure which previously led to -FI_EAGAIN posting.
    uint64_t base_caps = FI_MSG | FI_RMA | FI_READ | FI_REMOTE_READ;
    const char* force_write_caps = getenv("NIXL_OFI_FORCE_WRITE_CAPS");
    if (force_write_caps && (strcasecmp(force_write_caps, "1") == 0 || strcasecmp(force_write_caps, "true") == 0)) {
        base_caps |= FI_WRITE | FI_REMOTE_WRITE;
        NIXL_WARN << "OFI: forcing inclusion of WRITE/REMOTE_WRITE caps due to NIXL_OFI_FORCE_WRITE_CAPS";
    }
    hints->caps = base_caps;
    hints->mode = FI_CONTEXT;
    hints->addr_format = FI_FORMAT_UNSPEC;
    hints->tx_attr->tclass = 0x203;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->threading = FI_THREAD_DOMAIN;
    // prefer provider (auto) progress so that internal progress engines advance
    // resources without excessive manual polling during posting. we still poll
    // cqs explicitly on retry paths.
    hints->domain_attr->control_progress = FI_PROGRESS_AUTO;
    hints->domain_attr->data_progress = FI_PROGRESS_AUTO;
    hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
    // Default (lean) mr_mode: mirror common verbs;ofi_rxm result: LOCAL | VIRT_ADDR | ALLOCATED | PROV_KEY
    // RAW/ENDPOINT proved unnecessary and may cause provider negotiation friction; enable via strict env.
    bool strict_mr = false;
    if (const char* strict_env = getenv("NIXL_OFI_STRICT_MR")) {
        if (strcasecmp(strict_env, "1") == 0 || strcasecmp(strict_env, "true") == 0) strict_mr = true;
    }
    if (strict_mr) {
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY
#ifdef FI_MR_RAW
            | FI_MR_RAW
#endif
#ifdef FI_MR_ENDPOINT
            | FI_MR_ENDPOINT
#endif
            ;
        NIXL_WARN << "OFI: STRICT MR mode enabled (NIXL_OFI_STRICT_MR)";
    } else {
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    }

    if (!strict_mr) {
        NIXL_INFO << "OFI: Using relaxed mr_mode (override with NIXL_OFI_STRICT_MR=1)";
    }

    // Set provider name if not already set via environment variable
    const char* env_provider = getenv("FI_PROVIDER");
    NIXL_INFO << "OFI: Environment FI_PROVIDER = " << (env_provider ? env_provider : "not set");
    std::cerr << "OFI_DEBUG: env_provider check done\n";

    if (!env_provider) {
        if (!hints->fabric_attr) {
            // Fabric attr absent - allocate zeroed struct to be safe
            hints->fabric_attr = (fi_fabric_attr*)calloc(1, sizeof(fi_fabric_attr));
            if (!hints->fabric_attr) {
                NIXL_ERROR << "OFI: Failed to allocate fabric_attr for provider name";
                fi_freeinfo(hints);
                this->initErr = true;
                return;
            }
        }
        hints->fabric_attr->prov_name = strdup("verbs;ofi_rxm");
        if (!hints->fabric_attr->prov_name) {
            NIXL_ERROR << "OFI: Failed to allocate memory for provider name";
            fi_freeinfo(hints);
            this->initErr = true;
            return;
        }
        NIXL_INFO << "OFI: Set default provider to 'verbs;ofi_rxm'";
        std::cerr << "OFI_DEBUG: set default provider\n";
    }

    NIXL_INFO << "OFI: Calling fi_getinfo with FI_VERSION(1, 20)";
    std::cerr << "OFI_DEBUG: before fi_getinfo\n";
    auto logHints = [&](const char* phase){
        NIXL_INFO << phase << " caps=0x" << std::hex << hints->caps << std::dec
                  << " (" << decodeCaps(hints->caps) << ") ep_type=" << hints->ep_attr->type
                  << " mr_mode=0x" << std::hex << hints->domain_attr->mr_mode << std::dec
                  << " (" << decodeMrMode(hints->domain_attr->mr_mode) << ")";
    };
    logHints("OFI: Hints -");
    NIXL_INFO << "OFI: Hints - provider: " << (hints->fabric_attr->prov_name ? hints->fabric_attr->prov_name : "any");

    int ret = 0;
    const int max_info_retries = 3;
    bool relaxed_after_fail = false;
    for (int attempt = 0; attempt < max_info_retries; ++attempt) {
        ret = fi_getinfo(FI_VERSION(1, 20), nullptr, nullptr, 0, hints, &fi_);
        if (!ret) break;
        if (ret == -FI_ENODATA) {
            NIXL_WARN << "OFI: fi_getinfo returned -FI_ENODATA attempt=" << attempt;
            if (!relaxed_after_fail && !(getenv("NIXL_OFI_STRICT_MR"))) {
                // Drop PROV_KEY on retry if still failing (progressive relaxation)
                uint64_t before = hints->domain_attr->mr_mode;
                if (hints->domain_attr->mr_mode & FI_MR_PROV_KEY) {
                    hints->domain_attr->mr_mode &= ~FI_MR_PROV_KEY;
                    NIXL_WARN << "OFI: Relaxing mr_mode removing PROV_KEY (0x" << std::hex << before << " -> 0x" << hints->domain_attr->mr_mode << std::dec << ")";
                    relaxed_after_fail = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
            continue;
        } else {
            break; // other error
        }
    }
    if (ret) {
        NIXL_ERROR << "OFI: fi_getinfo failed after retries error " << ret << ": " << fi_strerror(-ret);
        fi_freeinfo(hints);
        this->initErr = true;
        return;
    }
    std::cerr << "OFI_DEBUG: after fi_getinfo\n";

    NIXL_INFO << "OFI: fi_getinfo succeeded";
    NIXL_INFO << "OFI: Selected provider: " << fi_->fabric_attr->prov_name;
    NIXL_INFO << "OFI: Domain name: " << (fi_->domain_attr->name ? fi_->domain_attr->name : "unspecified");
    // log returned provider capability + mode summary
    NIXL_INFO << "OFI: Provider caps=0x" << std::hex << fi_->caps << std::dec
              << " (" << decodeCaps(fi_->caps) << ") mode=0x" << std::hex << fi_->mode << std::dec
              << " mr_mode=0x" << std::hex << fi_->domain_attr->mr_mode << std::dec
              << " (" << decodeMrMode(fi_->domain_attr->mr_mode) << ")";
    if (fi_->tx_attr) {
        NIXL_INFO << "OFI: tx_attr size=" << fi_->tx_attr->size
                  << " inject_size=" << fi_->tx_attr->inject_size
                  << " iov_limit=" << fi_->tx_attr->iov_limit
                  << " rma_iov_limit=" << fi_->tx_attr->rma_iov_limit
                  << " op_flags=0x" << std::hex << fi_->tx_attr->op_flags << std::dec;
    }
    if (fi_->rx_attr) {
        NIXL_INFO << "OFI: rx_attr size=" << fi_->rx_attr->size
                  << " iov_limit=" << fi_->rx_attr->iov_limit
                  << " op_flags=0x" << std::hex << fi_->rx_attr->op_flags << std::dec;
    }
    if (fi_->ep_attr) {
        NIXL_INFO << "OFI: ep_attr max_msg_size=" << fi_->ep_attr->max_msg_size
                  << " auth_key_size=" << fi_->ep_attr->auth_key_size;
    }
    if (!(fi_->caps & FI_RMA)) {
        NIXL_WARN << "OFI: provider does not report FI_RMA – RMA ops will fail";
    }
    if (!(fi_->caps & FI_READ)) {
        NIXL_WARN << "OFI: provider missing FI_READ – read ops likely unsupported";
    }
    if (!(fi_->caps & FI_REMOTE_READ)) {
        NIXL_WARN << "OFI: provider missing FI_REMOTE_READ – remote read permissions may fail";
    }
    fi_freeinfo(hints);

    ret = fi_fabric(fi_->fabric_attr, &fabric_, nullptr);
    std::cerr << "OFI_DEBUG: before fi_fabric\n";
    if (ret) {
        NIXL_ERROR << "OFI: fi_fabric failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    std::cerr << "OFI_DEBUG: after fi_fabric\n";
    NIXL_INFO << "OFI: fi_fabric succeeded";

    ret = fi_domain(fabric_, fi_, &domain_, nullptr);
    std::cerr << "OFI_DEBUG: before fi_domain\n";
    if (ret) {
        NIXL_ERROR << "OFI: fi_domain failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    std::cerr << "OFI_DEBUG: after fi_domain\n";
    NIXL_INFO << "OFI: fi_domain succeeded";

    ret = fi_endpoint(domain_, fi_, &ep_, nullptr);
    std::cerr << "OFI_DEBUG: before fi_endpoint\n";
    if (ret) {
        NIXL_ERROR << "OFI: fi_endpoint failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    std::cerr << "OFI_DEBUG: after fi_endpoint\n";
    NIXL_INFO << "OFI: fi_endpoint succeeded";

    // Create separate TX and RX completion queues like server_bw.c
    struct fi_cq_attr cq_attr = {};
    cq_attr.size = 2048;
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;

    NIXL_INFO << "OFI: Creating TX completion queue with size " << cq_attr.size;
    ret = fi_cq_open(domain_, &cq_attr, &txcq_, nullptr);
    if (ret) {
        NIXL_ERROR << "OFI: fi_cq_open (TX) failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    NIXL_INFO << "OFI: TX completion queue created successfully";

    NIXL_INFO << "OFI: Creating RX completion queue with size " << cq_attr.size;
    ret = fi_cq_open(domain_, &cq_attr, &rxcq_, nullptr);
    if (ret) {
        NIXL_ERROR << "OFI: fi_cq_open (RX) failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    NIXL_INFO << "OFI: RX completion queue created successfully";

    // Bind TX and RX CQs separately
    NIXL_INFO << "OFI: Binding TX completion queue to endpoint";
    ret = fi_ep_bind(ep_, &txcq_->fid, FI_TRANSMIT);
    if (ret) {
        NIXL_ERROR << "OFI: fi_ep_bind (TX CQ) failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }

    NIXL_INFO << "OFI: Binding RX completion queue to endpoint";
    ret = fi_ep_bind(ep_, &rxcq_->fid, FI_RECV);
    if (ret) {
        NIXL_ERROR << "OFI: fi_ep_bind (RX CQ) failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }

    struct fi_av_attr av_attr = {};
    av_attr.type = FI_AV_TABLE;
    av_attr.count = 8; // allow multiple remote peers
    NIXL_INFO << "OFI: Creating address vector with type FI_AV_TABLE, count = " << av_attr.count;
    ret = fi_av_open(domain_, &av_attr, &av_, nullptr);
    if (ret) {
        NIXL_ERROR << "OFI: fi_av_open failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }
    NIXL_INFO << "OFI: Address vector created successfully";

    NIXL_INFO << "OFI: Binding address vector to endpoint";
    ret = fi_ep_bind(ep_, &av_->fid, 0);
    if (ret) {
        NIXL_ERROR << "OFI: fi_ep_bind (AV) failed with error " << ret << ": " << fi_strerror(-ret);
        this->initErr = true;
        return;
    }

    const char* delay_env = getenv("NIXL_OFI_DELAY_ENABLE");
    if (delay_env && (std::string(delay_env) == "1" || strcasecmp(delay_env, "true") == 0)) {
        delayed_enable_mode_ = true;
        NIXL_WARN << "OFI: delaying fi_enable due to NIXL_OFI_DELAY_ENABLE env var";
    } else {
        NIXL_INFO << "OFI: Enabling endpoint";
        ret = fi_enable(ep_);
        if (ret) {
            NIXL_ERROR << "OFI: fi_enable failed with error " << ret << ": " << fi_strerror(-ret);
            this->initErr = true;
            ep_ready_ = false;
            return;
        }
        ep_ready_ = true;
        ep_enabled_once_ = true;
        NIXL_INFO << "OFI: Endpoint enabled successfully";
        // Initialize persistent receive buffer for handshakes
        persistent_recv_buf_.fill(0);
        NIXL_INFO << "OFI: Initialized persistent receive buffer for handshakes";
        postPersistentRecv();
    }

    // Optional env to disable use of LOCAL sentinel so that we always expose a real provider address
    if (const char* dis = getenv("NIXL_OFI_DISABLE_LOCAL_SENTINEL")) {
        if (std::string(dis) == "1" || strcasecmp(dis, "true") == 0) {
            disable_local_sentinel_ = true;
            NIXL_WARN << "OFI: disabling LOCAL sentinel due to NIXL_OFI_DISABLE_LOCAL_SENTINEL env var";
            // If caller also requested delayed enable, ensure we late-enable now so getConnInfo has an address
            if (delayed_enable_mode_) {
                nixl_status_t late = ensureEndpointEnabled();
                if (late != NIXL_SUCCESS) {
                    NIXL_ERROR << "OFI: failed to late-enable endpoint while disabling sentinel";
                }
            }
        }
    }

    size_t addrlen = 256;
    std::vector<char> addr_buf(addrlen);
    if (!delayed_enable_mode_) {
        NIXL_INFO << "OFI: Getting endpoint address";
        ret = fi_getname(&ep_->fid, addr_buf.data(), &addrlen);
        if (ret) {
            NIXL_ERROR << "OFI: fi_getname failed with error " << ret << ": " << fi_strerror(-ret);
            this->initErr = true;
            return;
        }
    } else {
        if (disable_local_sentinel_) {
            NIXL_INFO << "OFI: delayed enable active but sentinel disabled — forcing early late-enable for address fetch";
            nixl_status_t late = ensureEndpointEnabled();
            if (late == NIXL_SUCCESS) {
                // ensureEndpointEnabled already fetched and logged address
            } else {
                NIXL_WARN << "OFI: ensureEndpointEnabled failed during constructor sentinel-disable path";
            }
        } else {
            NIXL_INFO << "OFI: skipping fi_getname until after delayed enable";
        }
    }
    // Defensive: cap returned addrlen to our buffer size to avoid huge allocations
    size_t safe_len = addrlen;
    if (safe_len > addr_buf.size()) {
        NIXL_WARN << "OFI: fi_getname returned addrlen=" << addrlen << " larger than buffer size=" << addr_buf.size() << ", capping";
        safe_len = addr_buf.size();
    }
    // Always construct from raw bytes and explicit length because address may contain NULs
    if (!delayed_enable_mode_) {
        localAddr_ = std::string(addr_buf.data(), safe_len);
        logAddressInfo("OFI: Local address", addr_buf.data(), addrlen, fi_->addr_format);
    }
    NIXL_INFO << "OFI: OFI backend initialization completed successfully";
    this->initErr = false;
}

void nixlOfiEngine::logAddressInfo(const std::string& prefix, const void* addr_data, size_t addr_len, int addr_format) const {
    NIXL_INFO << prefix << " length: " << addr_len << " bytes";

    // print hex representation
    std::ostringstream hex_stream;
    hex_stream << "0x";
    const unsigned char* bytes = static_cast<const unsigned char*>(addr_data);
    for (size_t i = 0; i < addr_len && i < 32; ++i) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2)
                   << static_cast<unsigned int>(bytes[i]);
        if (i % 4 == 3) hex_stream << " ";
    }
    if (addr_len > 32) hex_stream << "... (truncated)";
    NIXL_INFO << prefix << " (hex): " << hex_stream.str();

    // use known format or try to detect
    int format_to_use = addr_format;
    if (format_to_use == -1 && fi_) {
        format_to_use = fi_->addr_format;
    }

    // try to parse as socket address
    if (addr_len >= sizeof(struct sockaddr)) {
        const struct sockaddr* sa = static_cast<const struct sockaddr*>(addr_data);
        NIXL_INFO << prefix << " socket address family: " << sa->sa_family;

        if (sa->sa_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in* sin = static_cast<const struct sockaddr_in*>(addr_data);
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip_str, INET_ADDRSTRLEN);
            NIXL_INFO << prefix << " IPv4 address: " << ip_str << ":" << ntohs(sin->sin_port);
        } else if (sa->sa_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
            const struct sockaddr_in6* sin6 = static_cast<const struct sockaddr_in6*>(addr_data);
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, INET6_ADDRSTRLEN);
            NIXL_INFO << prefix << " IPv6 address: [" << ip_str << "]:" << ntohs(sin6->sin6_port);
        }
    }

    // alternative parsing: network order IPv4 (16 bytes starting with 0x0200)
    if (addr_len >= 16 && bytes[0] == 0x02 && bytes[1] == 0x00) {
        uint16_t port = ntohs(*reinterpret_cast<const uint16_t*>(&bytes[2]));

        // IPv4 address typically at offset 4-7 for sockaddr_in
        if (addr_len >= 8) {
            struct in_addr addr;
            addr.s_addr = *reinterpret_cast<const uint32_t*>(&bytes[4]);
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
            NIXL_INFO << prefix << " decoded IPv4: " << ip_str << ":" << port;
        }
    }
}

// instrumentation helpers
void nixlOfiEngine::logRmaStats(const char* tag) const {
    NIXL_INFO << "OFI: RMA stats[" << tag << "] posted=" << rma_posted_.load()
              << " completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load();
}

void nixlOfiEngine::dumpCQErrors(struct fid_cq* cq, const char* which) const {
    if (!cq) return;
    struct fi_cq_err_entry err = {};
    int ret;
    int count = 0;
    while ((ret = fi_cq_readerr(cq, &err, 0)) > 0) {
        NIXL_ERROR << "OFI: CQ(" << which << ") error: err=" << err.err
                   << " (" << fi_strerror(err.err) << ") prov_errno=" << err.prov_errno
                   << " flags=0x" << std::hex << err.flags << std::dec
                   << " len=" << err.len;
        if (err.err_data && err.err_data_size) {
            // best-effort small hexdump first 32 bytes
            size_t dump = std::min<size_t>(err.err_data_size, 32);
            const unsigned char* b = static_cast<const unsigned char*>(err.err_data);
            std::ostringstream oss; oss << "data=";
            for (size_t i=0;i<dump;i++) {
                oss << std::hex << std::setw(2) << std::setfill('0') << (int)b[i];
                if (i%4==3) oss << ' ';
            }
            NIXL_ERROR << "OFI: CQ(" << which << ") err_data_size=" << err.err_data_size << " " << oss.str();
        }
        count++;
    }
    if (count==0) {
        NIXL_INFO << "OFI: CQ(" << which << ") no error entries";
    }
}

// Stub tracing helper (fabtests-focused instrumentation will use FT_DEBUG in fabtests; backend timing optional)
void nixlOfiEngine::traceStage(const std::string &stage, const std::string &detail) const {
    (void)stage; (void)detail; // no-op until backend timing instrumentation is fully implemented
}

nixlOfiEngine::~nixlOfiEngine() {
    shutting_down_.store(true, std::memory_order_release);
    if (ep_) fi_close(&ep_->fid);
    if (av_) fi_close(&av_->fid);
    if (txcq_) fi_close(&txcq_->fid);
    if (rxcq_) fi_close(&rxcq_->fid);
    if (domain_) fi_close(&domain_->fid);
    if (fabric_) fi_close(&fabric_->fid);
    if (fi_) fi_freeinfo(fi_);
}

nixl_mem_list_t nixlOfiEngine::getSupportedMems() const {
    return {DRAM_SEG};
}

nixl_status_t nixlOfiEngine::connect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(avLock_);
    NIXL_INFO << "OFI: Attempting to connect to remote agent: " << remote_agent;

    // force-local override for debugging provider handshake issues
    if (const char* force_local = getenv("NIXL_OFI_FORCE_LOCAL")) {
        if (std::string(force_local) == "1" || strcasecmp(force_local, "true") == 0) {
            avAddrs_[remote_agent] = FI_ADDR_UNSPEC;
            NIXL_WARN << "OFI: NIXL_OFI_FORCE_LOCAL active – treating '" << remote_agent << "' as local";
            return NIXL_SUCCESS;
        }
    }

    if (avAddrs_.count(remote_agent)) {
        NIXL_INFO << "OFI: Already connected to " << remote_agent;
        return NIXL_SUCCESS;
    }

    // check for local connection during agent init
    auto it = remoteAddrs_.find(remote_agent);
    if (it == remoteAddrs_.end()) {
        // Remote hasn\'t provided conn info yet
        if (delayed_enable_mode_ && !ep_enabled_once_ && remote_agent != localAgent) {
            NIXL_INFO << "OFI: Remote agent " << remote_agent << " address not yet exchanged; deferring (no local fallback)";
            return NIXL_ERR_NOT_FOUND;
        }
        if (remote_agent == localAgent) {
            NIXL_INFO << "OFI: Self-connect detected ('" << remote_agent << "'), using local path";
            avAddrs_[remote_agent] = FI_ADDR_UNSPEC;
            return NIXL_SUCCESS;
        }
    }

    // For actual remote connections, proceed with normal handshake
    // local connection handled above

    // log remote address details (handle sentinel)
    const std::string& remote_addr_str = it->second;
    if (remote_addr_str.empty()) {
        NIXL_INFO << "OFI: Remote agent provided empty conn_info (address not ready)";
        return NIXL_ERR_NOT_FOUND;
    }
    if (remote_addr_str == "LOCAL") {
        if (disable_local_sentinel_) {
            NIXL_WARN << "OFI: LOCAL sentinel received but disabled; waiting for real address";
            return NIXL_ERR_NOT_FOUND;
        } else {
            NIXL_INFO << "OFI: Remote agent " << remote_agent << " provided sentinel LOCAL address; using local fallback path";
            avAddrs_[remote_agent] = FI_ADDR_UNSPEC;
            handshake_completed_.insert(remote_agent);
            return NIXL_SUCCESS;
        }
    }
    logAddressInfo("OFI: Remote address", remote_addr_str.data(), remote_addr_str.length(), fi_->addr_format);

    fi_addr_t addr;
    NIXL_INFO << "OFI: Inserting remote address into AV (FI_AV_TABLE)";
    int ret = fi_av_insert(av_, it->second.data(), 1, &addr, 0, nullptr);
    if (ret != 1) {
        NIXL_ERROR << "OFI: fi_av_insert failed, returned " << ret << " (expected 1)";
        return NIXL_ERR_BACKEND;
    }
    avAddrs_[remote_agent] = addr;
    NIXL_INFO << "OFI: Successfully connected to " << remote_agent << ", AV table index: " << addr;

    // Perform delayed enable now (only once) if requested via env var
    if (delayed_enable_mode_ && !ep_enabled_once_) {
        NIXL_INFO << "OFI: performing deferred fi_enable after AV insertion";
        int enret = fi_enable(ep_);
        if (enret) {
            NIXL_ERROR << "OFI: deferred fi_enable failed: " << enret << " (" << fi_strerror(-enret) << ")";
            return NIXL_ERR_BACKEND;
        }
        ep_ready_ = true;
        ep_enabled_once_ = true;
        // now fetch and log address
        size_t addrlen = 256;
        std::vector<char> buf(addrlen);
        int gnret = fi_getname(&ep_->fid, buf.data(), &addrlen);
        if (gnret) {
            NIXL_WARN << "OFI: deferred fi_getname failed: " << gnret << " (" << fi_strerror(-gnret) << ")";
        } else {
            size_t safe_len = std::min(addrlen, buf.size());
            localAddr_ = std::string(buf.data(), safe_len);
            logAddressInfo("OFI: Local address (deferred)", buf.data(), addrlen, fi_->addr_format);
        }
        // initialize handshake recv buffer now
        persistent_recv_buf_.fill(0);
        postPersistentRecv();
        NIXL_INFO << "OFI: deferred enable complete";
    }

    // Perform handshake to ensure connection is fully established
    nixl_status_t handshake_status = performHandshake(remote_agent, addr);
    if (handshake_status != NIXL_SUCCESS) {
        NIXL_ERROR << "OFI: Handshake failed with " << remote_agent;
        // Cleanup the connection
        fi_av_remove(av_, &addr, 1, 0);
        avAddrs_.erase(remote_agent);
        return handshake_status;
    }

    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::ensureEndpointEnabled() {
    if (ep_enabled_once_) {
        return NIXL_SUCCESS;
    }
    std::lock_guard<std::mutex> elock(enableLock_);
    if (ep_enabled_once_) return NIXL_SUCCESS; // double-checked
    if (!delayed_enable_mode_) {
        // Should not happen: endpoint should already be enabled in non-delayed mode
        if (!ep_ready_) {
            NIXL_ERROR << "OFI: ensureEndpointEnabled called but endpoint not ready and not in delayed mode";
            return NIXL_ERR_BACKEND;
        }
        return NIXL_SUCCESS;
    }
    NIXL_INFO << "OFI: Late enabling endpoint (ensureEndpointEnabled)";
    int ret = fi_enable(ep_);
    if (ret) {
        NIXL_ERROR << "OFI: fi_enable (late) failed: " << ret << " (" << fi_strerror(-ret) << ")";
        return NIXL_ERR_BACKEND;
    }
    ep_ready_ = true;
    ep_enabled_once_ = true;
    // fetch address
    size_t addrlen = 256; std::vector<char> buf(addrlen);
    int gnret = fi_getname(&ep_->fid, buf.data(), &addrlen);
    if (gnret) {
        NIXL_WARN << "OFI: fi_getname (late) failed: " << gnret << " (" << fi_strerror(-gnret) << ")";
    } else {
        size_t safe_len = std::min(addrlen, buf.size());
        localAddr_ = std::string(buf.data(), safe_len);
        logAddressInfo("OFI: Local address (late)", buf.data(), addrlen, fi_->addr_format);
    }
    // initialize persistent recv for handshake path
    persistent_recv_buf_.fill(0);
    postPersistentRecv();
    NIXL_INFO << "OFI: Late enable complete";
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::disconnect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(avLock_);
    auto it = avAddrs_.find(remote_agent);
    if (it == avAddrs_.end()) {
        return NIXL_ERR_NOT_FOUND;
    }
    fi_av_remove(av_, &it->second, 1, 0);
    avAddrs_.erase(it);
    // Remove from handshake completed set
    handshake_completed_.erase(remote_agent);
    return NIXL_SUCCESS;
}

// post persistent receive buffer for rxm wakeup messages (can be called multiple times)
nixl_status_t nixlOfiEngine::postPersistentRecv() const {
    if (!ep_ || !rxcq_) {
        NIXL_ERROR << "OFI: Endpoint or RX CQ not initialized";
        return NIXL_ERR_BACKEND;
    }

    // Initialize fresh recv buffer for wakeup/handshake messages
    persistent_recv_buf_.fill(0);

    NIXL_DEBUG << "OFI: Posting fresh persistent recv buffer for RXM wakeup/handshake";

    int ret = fi_recv(ep_, persistent_recv_buf_.data(), WAKEUP_MSG_SIZE,
                     nullptr, FI_ADDR_UNSPEC, const_cast<uint64_t*>(&recv_context_));

    if (ret && ret != -FI_EAGAIN) {
        NIXL_DEBUG << "OFI: fi_recv for persistent buffer failed: " << fi_strerror(-ret)
                   << " (ret=" << ret << ")";
        return NIXL_ERR_BACKEND;
    }

    NIXL_DEBUG << "OFI: Posted fresh persistent recv buffer successfully";
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::performHandshake(const std::string &remote_agent, fi_addr_t remote_addr) const {
    // Check if handshake already completed
    if (handshake_completed_.count(remote_agent)) {
        NIXL_DEBUG << "OFI: Handshake already completed with " << remote_agent;
        return NIXL_SUCCESS;
    }

    NIXL_INFO << "OFI: Starting handshake with " << remote_agent;

    // Post a receive buffer to ensure we can receive data/notifications
    nixl_status_t recv_status = postPersistentRecv();
    if (recv_status != NIXL_SUCCESS) {
        NIXL_ERROR << "OFI: Failed to post receive buffer for handshake";
        return recv_status;
    }

    // Actively send a tiny token to trigger provider-level connection setup (needed for rxm + verbs before RMA)
    bool degraded_local_fallback = false;
    // If set (default true) we will NOT force a local fallback on handshake failure; instead we
    // treat the handshake as best-effort and allow RMA ops to trigger provider connection lazily.
    // Set NIXL_OFI_REQUIRE_HANDSHAKE=1 to restore strict behavior that downgrades to local path.
    bool require_handshake = false;
    if (const char* req = getenv("NIXL_OFI_REQUIRE_HANDSHAKE")) {
        if (strcasecmp(req, "1") == 0 || strcasecmp(req, "true") == 0) require_handshake = true;
    }
    if (remote_addr != FI_ADDR_UNSPEC) {
        char token = 0xAB;
        int retries = 0;
        const int max_send_retries = 60; // extend up to ~300ms total (5ms backoff after phase 1)
        int sret;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            sret = fi_send(ep_, &token, sizeof(token), nullptr, remote_addr, nullptr);
            if (sret == 0) {
                NIXL_INFO << "OFI: handshake fi_send token posted (retries=" << retries << ")";
                break;
            } else if (sret == -FI_EAGAIN) {
                retries++;
                if (retries % 5 == 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    NIXL_DEBUG << "OFI: handshake still EAGAIN retries=" << retries << " elapsed_ms=" << elapsed;
                    // Attempt to pull any provider errors for added diagnostics
                    struct fi_cq_err_entry err = {};
                    int er;
                    if (txcq_) {
                        er = fi_cq_readerr(txcq_, &err, 0);
                        if (er > 0) {
                            NIXL_WARN << "OFI: TX CQ error during handshake EAGAIN: err=" << err.err << " prov=" << (err.prov_errno) ;
                        }
                    }
                    if (rxcq_) {
                        er = fi_cq_readerr(rxcq_, &err, 0);
                        if (er > 0) {
                            NIXL_WARN << "OFI: RX CQ error during handshake EAGAIN: err=" << err.err << " prov=" << (err.prov_errno) ;
                        }
                    }
                }
                // progress cqs
                struct fi_cq_entry comp;
                while (fi_cq_read(txcq_, &comp, 1) > 0) {}
                while (fi_cq_read(rxcq_, &comp, 1) > 0) {}
                if (retries >= max_send_retries) {
                    if (require_handshake) {
                        NIXL_WARN << "OFI: handshake fi_send never posted after " << retries << " retries; degrading to local memcpy fallback (strict mode)";
                        degraded_local_fallback = true;
                    } else {
                        NIXL_WARN << "OFI: handshake fi_send not posted after " << retries << " retries; proceeding WITHOUT downgrade (best-effort handshake)";
                    }
                    break;
                }
                // small backoff after the first few tight spins
                int backoff_ms = (retries < 10) ? 1 : 5;
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                continue;
            } else {
                NIXL_ERROR << "OFI: handshake fi_send failed: " << fi_strerror(-sret) << " (ret=" << sret << ")";
                return NIXL_ERR_BACKEND;
            }
        }
    } else {
        NIXL_DEBUG << "OFI: skipping handshake send for local connection";
    }

    if (degraded_local_fallback) {
        // mark this remote as local fallback (fi_addr_t = FI_ADDR_UNSPEC) so transfer path uses memcpy
        std::lock_guard<std::mutex> lock(avLock_);
        auto it = avAddrs_.find(remote_agent);
        if (it != avAddrs_.end()) {
            it->second = FI_ADDR_UNSPEC;
        }
        handshake_completed_.insert(remote_agent);
        NIXL_WARN << "OFI: handshake degraded for " << remote_agent << " using local memcpy path";
        return NIXL_SUCCESS;
    }

    // Give the connection a moment to stabilize (provider may perform async CM handshakes)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Mark handshake as completed
    handshake_completed_.insert(remote_agent);
    NIXL_INFO << "OFI: Handshake completed successfully with " << remote_agent;

    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::registerMem(const nixlBlobDesc &mem,
                                             const nixl_mem_t &nixl_mem,
                                             nixlBackendMD* &out) {
    NIXL_INFO << "OFI: registerMem called with addr=" << std::hex << mem.addr
              << ", len=" << std::dec << mem.len << ", nixl_mem=" << nixl_mem;

    if (nixl_mem != DRAM_SEG) {
        NIXL_ERROR << "OFI: registerMem unsupported memory type: " << nixl_mem;
        return NIXL_ERR_NOT_SUPPORTED;
    }

    auto meta = new nixlOfiMetadata();
    NIXL_INFO << "OFI: Calling fi_mr_reg with domain=" << domain_
              << ", addr=" << std::hex << (void*)mem.addr
              << ", len=" << std::dec << mem.len;

    // Access flags: include local READ/WRITE plus remote READ/WRITE capabilities.
    // Omit FI_SEND/FI_RECV to reduce resource class requirements (fabtests parity).
    uint64_t access = FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    int ret = fi_mr_reg(domain_, (void*)mem.addr, mem.len, access, 0, 0, 0, &meta->mr, nullptr);
    if (ret) {
        NIXL_ERROR << "OFI: fi_mr_reg failed (access=0x" << std::hex << access << std::dec << ")";
    }
    if (ret) {
        NIXL_ERROR << "OFI: fi_mr_reg failed with error " << ret << ": " << fi_strerror(-ret);
        delete meta;
        return NIXL_ERR_BACKEND;
    }

    meta->desc = fi_mr_desc(meta->mr);
    meta->remote_key = fi_mr_key(meta->mr);  // Store the remote key for cross-agent access
    NIXL_INFO << "OFI: Memory registration successful, mr=" << meta->mr << ", desc=" << meta->desc << ", remote_key=" << meta->remote_key;
    out = meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::deregisterMem(nixlBackendMD *meta) {
    auto ofi_meta = static_cast<nixlOfiMetadata*>(meta);
    if (ofi_meta) {
        if (ofi_meta->mr && ofi_meta->mr->fid.fclass == FI_CLASS_MR) {
            // Check if the memory region is still valid before closing
            NIXL_DEBUG << "OFI: Closing memory region " << ofi_meta->mr;
            int ret = fi_close(&ofi_meta->mr->fid);
            if (ret) {
                NIXL_ERROR << "OFI: fi_close failed for memory region: " << ret << " (" << fi_strerror(-ret) << ")";
                // Don't return error, continue with cleanup
            } else {
                NIXL_DEBUG << "OFI: Successfully closed memory region";
            }
            ofi_meta->mr = nullptr;
        } else if (ofi_meta->mr) {
            NIXL_WARN << "OFI: Memory region pointer invalid, skipping fi_close";
            ofi_meta->mr = nullptr;
        }
        delete ofi_meta;
    }
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
    if (local.descCount() == 0 || remote.descCount() == 0) {
        return NIXL_ERR_INVALID_PARAM;
    }

    // Create request handle for the transfer operation
    handle = new nixlOfiRequest(local.descCount());
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::postXfer(const nixl_xfer_op_t &operation,
                                          const nixl_meta_dlist_t &local,
                                          const nixl_meta_dlist_t &remote,
                                          const std::string &remote_agent,
                                          nixlBackendReqH* &handle,
                                          const nixl_opt_b_args_t* opt_args) const {
    fi_addr_t dest_addr;
    bool is_local_operation = false;

    // Check if we're already connected - if not, try to connect
    bool need_connect = false;
    {
        std::lock_guard<std::mutex> lock(avLock_);
        auto it = avAddrs_.find(remote_agent);
        if (it == avAddrs_.end()) {
            need_connect = true;
        } else {
            dest_addr = it->second;
            // local operation if dest_addr == FI_ADDR_UNSPEC
            is_local_operation = (dest_addr == FI_ADDR_UNSPEC);
        }
    }

    if (need_connect) {
        NIXL_ERROR << "OFI: Remote agent '" << remote_agent
                   << "' has no resolved address (auto-connect disabled). Call connect() explicitly before posting xfer.";
        return NIXL_ERR_NOT_FOUND; // escalate to caller; no silent fallback
    }


    // Get the destination address (works for both new and existing connections)
    {
        std::lock_guard<std::mutex> lock(avLock_);
        auto it = avAddrs_.find(remote_agent);
        if (it == avAddrs_.end()) {
            return NIXL_ERR_NOT_FOUND;
        }
        dest_addr = it->second;
        is_local_operation = (dest_addr == FI_ADDR_UNSPEC);
        NIXL_INFO << "OFI: Using remote address (fi_addr_t) for agent '" << remote_agent << "': " << dest_addr
                  << (is_local_operation ? " (local operation)" : "");
    }

    // Require real remote path for non-local agents. Only allow memcpy optimization
    // when the caller explicitly targets the same local agent name. If the
    // fi_addr_t is FI_ADDR_UNSPEC for a different agent, treat as an error so
    // higher layers see a failed remote transfer instead of a silent fallback.
    if (!ep_ready_) {
        nixl_status_t en = const_cast<nixlOfiEngine*>(this)->ensureEndpointEnabled();
        if (en != NIXL_SUCCESS) {
            NIXL_ERROR << "OFI: endpoint not ready and late enable failed";
            return en;
        }
    }
    if (is_local_operation) {
        if (remote_agent != localAgent) {
            NIXL_ERROR << "OFI: Refusing memcpy fallback for remote agent '" << remote_agent
                       << "' (localAgent='" << localAgent << "'). Address unresolved (FI_ADDR_UNSPEC).";
            return NIXL_ERR_NOT_FOUND; // propagate as remote resolution failure
        }
        NIXL_INFO << "OFI: Local self-transfer detected (agent='" << remote_agent << "'), performing memcpy optimization";
        auto req = new nixlOfiRequest(local.descCount());
        handle = req;
        for (int i = 0; i < local.descCount(); ++i) {
            if (operation == NIXL_READ) {
                std::memcpy((void*)local[i].addr, (void*)remote[i].addr, local[i].len);
            } else if (operation == NIXL_WRITE) {
                std::memcpy((void*)remote[i].addr, (void*)local[i].addr, local[i].len);
            } else {
                delete req;
                return NIXL_ERR_NOT_SUPPORTED;
            }
            req->completed_operations++;
        }
        return NIXL_SUCCESS;
    }

    auto req = new nixlOfiRequest(local.descCount());
    handle = req;

    NIXL_INFO << "OFI: Starting transfer loop with " << local.descCount() << " descriptors (rma_posted=" << rma_posted_.load() << " rma_completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load() << ")";
    bool posted_async = false;
    for (int i = 0; i < local.descCount(); ++i) {
    NIXL_INFO << "OFI: Processing descriptor " << i << " (posted=" << rma_posted_.load() << " completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load() << ")";
        auto local_meta = static_cast<nixlOfiMetadata*>(local[i].metadataP);
        auto remote_meta = static_cast<nixlOfiMetadata*>(remote[i].metadataP);

        if (!local_meta) {
            NIXL_ERROR << "OFI: local metadata is null for descriptor " << i;
            return NIXL_ERR_INVALID_PARAM;
        }
        if (!remote_meta) {
            NIXL_ERROR << "OFI: remote metadata is null for descriptor " << i;
            return NIXL_ERR_INVALID_PARAM;
        }

        NIXL_INFO << "OFI: remote metadata pointer for descriptor " << i << ": " << std::hex << reinterpret_cast<uintptr_t>(remote_meta) << std::dec;
        NIXL_INFO << "OFI: local buffer address: " << std::hex << local[i].addr << std::dec << ", length: " << local[i].len;
        NIXL_INFO << "OFI: remote buffer address: " << std::hex << remote[i].addr << std::dec << ", length: " << remote[i].len;

        uint64_t remote_key = remote_meta->remote_key;  // Use the stored remote key
        if (remote_key == 0) {
            NIXL_ERROR << "OFI: invalid remote key for descriptor " << i;
            return NIXL_ERR_INVALID_PARAM;
        }
        NIXL_INFO << "OFI: remote key for descriptor " << i << ": " << remote_key;

        int ret;
        int retry_count = 0;
        const int max_retries = 10;  // Increased for inter-agent transfers

        do {
            if (operation == NIXL_READ) {
                NIXL_INFO << "OFI: Executing fi_read - desc=" << i << " retry=" << retry_count << " local=0x" << std::hex << local[i].addr
                          << " len=" << std::dec << local[i].len
                          << " remote=0x" << std::hex << remote[i].addr << std::dec
                          << " remote_key=" << remote_key;
                ret = fi_read(ep_, (void*)local[i].addr, local[i].len, local_meta->desc, dest_addr, remote[i].addr, remote_key, req);
                if (ret == 0) { rma_posted_++; }
            } else if (operation == NIXL_WRITE) {
                NIXL_INFO << "OFI: Executing fi_write - desc=" << i << " retry=" << retry_count << " local=0x" << std::hex << local[i].addr
                          << " len=" << std::dec << local[i].len
                          << " remote=0x" << std::hex << remote[i].addr << std::dec
                          << " remote_key=" << remote_key;
                ret = fi_write(ep_, (void*)local[i].addr, local[i].len, local_meta->desc, dest_addr, remote[i].addr, remote_key, req);
                if (ret == 0) { rma_posted_++; }
            } else {
                return NIXL_ERR_NOT_SUPPORTED;
            }

            if (ret == -FI_EAGAIN) {
                retry_count++;
                if (retry_count < max_retries) {
                    rma_eagain_++;
                    NIXL_INFO << "OFI: Retry " << retry_count << " after -FI_EAGAIN (desc=" << i << ") progressing CQs (posted=" << rma_posted_.load() << " completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load() << ")";

                    // Progress completion queues more aggressively
                    struct fi_cq_entry comp;
                    ssize_t tx_progress, rx_progress;
                    int total_completions = 0;

                    // Progress TX completions
                    do {
                        tx_progress = fi_cq_read(txcq_, &comp, 1);
                        if (tx_progress > 0) {
                            total_completions++;
                            NIXL_DEBUG << "OFI: Progressed TX completion during retry";
                        }
                    } while (tx_progress > 0);

                    // Progress RX completions
                    do {
                        rx_progress = fi_cq_read(rxcq_, &comp, 1);
                        if (rx_progress > 0) {
                            total_completions++;
                            NIXL_DEBUG << "OFI: Progressed RX completion during retry";
                        }
                    } while (rx_progress > 0);

                    if (total_completions > 0) {
                        NIXL_DEBUG << "OFI: Progressed " << total_completions << " completions, retrying immediately";
                        // If we made progress, retry immediately
                        continue;
                    }

                    // Exponential backoff: wait longer for later retries
                    int wait_ms = std::min(1 << (retry_count / 5), 10);  // 1ms -> 2ms -> 4ms -> 8ms -> 10ms max
                    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                } else {
                    NIXL_ERROR << "OFI: Max retries (" << max_retries << ") exceeded for descriptor " << i << ", dumping cq errors";
                    dumpCQErrors(txcq_, "TX");
                    dumpCQErrors(rxcq_, "RX");
                    break;
                }
            }
        } while (ret == -FI_EAGAIN && retry_count < max_retries);

    NIXL_INFO << "OFI: fi_" << (operation == NIXL_READ ? "read" : "write") << " returned: " << ret << " (posted=" << rma_posted_.load() << " completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load() << ")";
        if (ret == 0) {
            NIXL_INFO << "OFI: Operation completed immediately";
        } else if (ret > 0) {
            NIXL_INFO << "OFI: Operation posted successfully (will complete asynchronously)";
            posted_async = true;
        } else if (ret == -FI_EAGAIN) {
            NIXL_ERROR << "OFI: Operation failed to post (-FI_EAGAIN) - resource temporarily unavailable";
            delete req;
            handle = nullptr;
            return NIXL_ERR_BACKEND;
        } else {
            NIXL_ERROR << "OFI: fi_" << (operation == NIXL_READ ? "read" : "write") << " failed with error: " << ret << " (" << fi_strerror(-ret) << ")";
            delete req;
            handle = nullptr;
            return NIXL_ERR_BACKEND;
        }
    }
    return posted_async ? NIXL_IN_PROG : NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::checkXfer(nixlBackendReqH* handle) const {
    if (!handle) {
        NIXL_ERROR << "OFI: checkXfer called with null handle (previous post failure)";
        return NIXL_ERR_BACKEND;
    }
    auto req = static_cast<nixlOfiRequest*>(handle);
    NIXL_INFO << "OFI: checkXfer - completed=" << req->completed_operations
              << " total=" << req->total_operations
              << " (rma_posted=" << rma_posted_.load() << " rma_completed=" << rma_completed_.load() << " eagain=" << rma_eagain_.load() << ")";

    if (req->isComplete()) {
        NIXL_INFO << "OFI: Transfer is complete";
        return NIXL_SUCCESS;
    }

    struct fi_cq_entry comp;

    // Check TX completion queue
    ssize_t ret = fi_cq_read(txcq_, &comp, 1);
    NIXL_INFO << "OFI: TX CQ read returned: " << ret;
    if (ret > 0) {
        if (comp.op_context == req) {
            req->completed_operations++;
            rma_completed_++;
            NIXL_INFO << "OFI: TX operation completed, now completed=" << req->completed_operations;
        } else {
            NIXL_INFO << "OFI: TX completion for different request";
        }
    } else if (ret == -FI_EAGAIN) {
        // No completions yet: block until one shows up
        ssize_t sret = fi_cq_sread(txcq_, &comp, 1, NULL, 0);
        NIXL_INFO << "OFI: TX CQ sread returned: " << sret;
        if (sret > 0 && comp.op_context == req) {
            req->completed_operations++;
            rma_completed_++;
            NIXL_INFO << "OFI: TX operation completed (sread), now completed=" << req->completed_operations;
        }
    } else if (ret < 0) {
        struct fi_cq_err_entry err_entry;
        ssize_t err_ret = fi_cq_readerr(txcq_, &err_entry, 0);
        if (err_ret > 0) {
            NIXL_ERROR << "OFI: TX CQ error: " << fi_strerror(err_entry.err) << " (code=" << err_entry.err << ")";
        } else {
            NIXL_ERROR << "OFI: TX CQ read error: " << ret << " (" << fi_strerror(-ret) << ")";
        }
        return NIXL_ERR_BACKEND;
    }

    // Check RX completion queue
    ret = fi_cq_read(rxcq_, &comp, 1);
    NIXL_INFO << "OFI: RX CQ read returned: " << ret;
    if (ret > 0) {
        if (comp.op_context == req) {
            req->completed_operations++;
            rma_completed_++;
            NIXL_INFO << "OFI: RX operation completed, now completed=" << req->completed_operations;
        } else {
            NIXL_INFO << "OFI: RX completion for different request";
        }
    } else if (ret == -FI_EAGAIN) {
        // No RX completions yet: block until one shows up
        ssize_t sret = fi_cq_sread(rxcq_, &comp, 1, NULL, 0);
        NIXL_INFO << "OFI: RX CQ sread returned: " << sret;
        if (sret > 0 && comp.op_context == req) {
            req->completed_operations++;
            rma_completed_++;
            NIXL_INFO << "OFI: RX operation completed (sread), now completed=" << req->completed_operations;
        }
    } else if (ret < 0) {
        struct fi_cq_err_entry err_entry;
        ssize_t err_ret = fi_cq_readerr(rxcq_, &err_entry, 0);
        if (err_ret > 0) {
            NIXL_ERROR << "OFI: RX CQ error: " << fi_strerror(err_entry.err) << " (code=" << err_entry.err << ")";
        } else {
            NIXL_ERROR << "OFI: RX CQ read error: " << ret << " (" << fi_strerror(-ret) << ")";
        }
        return NIXL_ERR_BACKEND;
    }

    bool is_complete = req->isComplete();
    NIXL_INFO << "OFI: Transfer " << (is_complete ? "complete" : "in progress");
    return is_complete ? NIXL_SUCCESS : NIXL_IN_PROG;
}

nixl_status_t nixlOfiEngine::releaseReqH(nixlBackendReqH* handle) const {
    delete static_cast<nixlOfiRequest*>(handle);
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getConnInfo(std::string &conn_info) const {
    // Provide a non-empty sentinel if endpoint address not yet known (delayed enable path)
    // to avoid remote metadata load rejecting empty connection strings (NIXL_ERR_MISMATCH).
    if (localAddr_.empty()) {
        if (disable_local_sentinel_) {
            // Force caller to wait until we actually have an address; return empty (caller must handle retry)
            conn_info.clear();
            NIXL_INFO << "OFI: getConnInfo returning empty (sentinel disabled, address not ready)";
        } else {
            conn_info = "LOCAL"; // sentinel indicates intra-process/local fallback
        }
    } else {
        conn_info = localAddr_;
    }
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) {
    std::lock_guard<std::mutex> lock(avLock_);
    remoteAddrs_[remote_agent] = conn_info;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getPublicData(const nixlBackendMD* meta, std::string &str) const {
    auto ofi_meta = static_cast<const nixlOfiMetadata*>(meta);
    uint64_t key = fi_mr_key(ofi_meta->mr);
    str = std::to_string(key);
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteMD(const nixlBlobDesc &input, const nixl_mem_t &nixl_mem,
                                               const std::string &remote_agent, nixlBackendMD* &output) {
    auto meta = new nixlOfiMetadata();
    // Store the remote key from the serialized metadata
    meta->remote_key = std::stoull(std::string(input.metaInfo.begin(), input.metaInfo.end()));
    meta->mr = nullptr;  // Remote side doesn't have the actual mr object
    meta->desc = nullptr;  // Remote side doesn't have the actual descriptor
    NIXL_INFO << "OFI: loadRemoteMD - stored remote_key: " << meta->remote_key;
    output = meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadLocalMD(nixlBackendMD* input, nixlBackendMD* &output) {
    // For local operations, the input metadata can be used directly
    // No need to create a new metadata object since it's the same memory region
    NIXL_INFO << "OFI: loadLocalMD - using input metadata directly for local operations";
    output = input;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getNotifs(notif_list_t &notif_list) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return NIXL_SUCCESS;
    }
    if (!rxcq_) {
        return NIXL_SUCCESS;
    }
    if (!ep_ready_) {
        // Avoid touching RX CQ prior to fi_enable()/late enable sequence; provider internal
        // CM structures for rxm may not be fully active and fi_cq_read could trigger faults.
        NIXL_DEBUG << "OFI: getNotifs skipped (endpoint not yet enabled)";
        return NIXL_SUCCESS;
    }
    // check RX CQ for notifications
    struct fi_cq_data_entry comp;
    ssize_t ret = fi_cq_read(rxcq_, &comp, 1);

    if (ret > 0) {
    // found completions, create notifications
        for (ssize_t i = 0; i < ret; i++) {
            // create notification entry
            std::pair<std::string, std::string> notification;
            notification.first = ""; // OFI doesn't provide source info directly
            notification.second = "0"; // use completion data if available
            if (comp.flags & FI_REMOTE_CQ_DATA) {
                notification.second = std::to_string(comp.data);
            }
            notif_list.push_back(notification);

            NIXL_DEBUG << "OFI: getNotifs found completion, added notification with tag=" << notification.second;
        }

    // post new recv buffer after completion
        postPersistentRecv();

        NIXL_DEBUG << "OFI: getNotifs processed " << ret << " completions, notif_list size now " << notif_list.size();
        return NIXL_SUCCESS;
    } else if (ret == -FI_EAGAIN) {
    // no completions, normal
        return NIXL_SUCCESS;
    } else {
    // error reading CQ
        NIXL_DEBUG << "OFI: getNotifs error reading RX CQ: " << fi_strerror(-ret);
        return NIXL_SUCCESS; // Don't fail to maintain stability
    }
}
