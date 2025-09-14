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

#ifndef __OFI_BACKEND_H
#define __OFI_BACKEND_H

#include <nixl.h>
#include <nixl_types.h>
#include "backend/backend_engine.h"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <set>
#include <chrono>

class nixlOfiMetadata : public nixlBackendMD {
public:
    fid_mr *mr;
    void *desc;
    uint64_t remote_key;  // Store the remote key for cross-agent access

    nixlOfiMetadata() : nixlBackendMD(false), mr(nullptr), desc(nullptr), remote_key(0) {}
    ~nixlOfiMetadata() {}
};

class nixlOfiRequest : public nixlBackendReqH {
public:
    size_t total_operations;
    std::atomic<size_t> completed_operations;

    nixlOfiRequest(size_t total_ops) : total_operations(total_ops), completed_operations(0) {}
    ~nixlOfiRequest() {}

    bool isComplete() const { return completed_operations >= total_operations; }
};

class nixlOfiEngine : public nixlBackendEngine {
public:
    nixlOfiEngine(const nixlBackendInitParams* init_params);
    ~nixlOfiEngine();

    bool supportsNotif() const override { return true; }
    bool supportsRemote() const override { return true; }
        // Guard to prevent use-after-close during destruction (getNotifs may be called late)
        std::atomic<bool> shutting_down_{false};
    bool supportsLocal() const override { return true; }
    bool supportsProgTh() const override { return true; }

    nixl_mem_list_t getSupportedMems() const override;

    nixl_status_t connect(const std::string &remote_agent) override;
    nixl_status_t disconnect(const std::string &remote_agent) override;

    nixl_status_t registerMem(const nixlBlobDesc &mem,
                             const nixl_mem_t &nixl_mem,
                             nixlBackendMD* &out) override;
    nixl_status_t deregisterMem(nixlBackendMD *meta) override;
    nixl_status_t unloadMD(nixlBackendMD* input) override;

    nixl_status_t prepXfer(const nixl_xfer_op_t &operation,
                          const nixl_meta_dlist_t &local,
                          const nixl_meta_dlist_t &remote,
                          const std::string &remote_agent,
                          nixlBackendReqH* &handle,
                          const nixl_opt_b_args_t* opt_args=nullptr) const override;
    nixl_status_t postXfer(const nixl_xfer_op_t &operation,
                          const nixl_meta_dlist_t &local,
                          const nixl_meta_dlist_t &remote,
                          const std::string &remote_agent,
                          nixlBackendReqH* &handle,
                          const nixl_opt_b_args_t* opt_args=nullptr) const override;

    nixl_status_t checkXfer(nixlBackendReqH* handle) const override;
    nixl_status_t releaseReqH(nixlBackendReqH* handle) const override;

    nixl_status_t getConnInfo(std::string &conn_info) const override;
    nixl_status_t loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) override;

    nixl_status_t getPublicData(const nixlBackendMD* meta, std::string &str) const override;
    nixl_status_t loadRemoteMD(const nixlBlobDesc &input, const nixl_mem_t &nixl_mem,
                               const std::string &remote_agent, nixlBackendMD* &output) override;

    // local operations support
    nixl_status_t loadLocalMD(nixlBackendMD* input, nixlBackendMD* &output) override;

    // Notification support
    nixl_status_t getNotifs(notif_list_t &notif_list) override;

    // Ensure endpoint enabled (late enable path). Safe to call multiple times.
    nixl_status_t ensureEndpointEnabled();

private:
    fid_fabric *fabric_;
    fid_domain *domain_;
    fid_ep *ep_;
    fid_cq *txcq_;
    fid_cq *rxcq_;
    fid_av *av_;
    struct fi_info *fi_;
    bool ep_ready_ = false;
    bool delayed_enable_mode_ = false; // if true we delay fi_enable until after AV insert
    bool ep_enabled_once_ = false;     // track if enable has been performed
    mutable std::mutex enableLock_;    // guard late enable sequence
    bool disable_local_sentinel_ = false; // when true we never emit/accept LOCAL sentinel (forces real address exchange)
    bool timing_enabled_ = false; // enabled via NIXL_OFI_TIMING env var
    std::chrono::steady_clock::time_point start_time_;
    mutable std::mutex trace_lock_;
    mutable std::set<std::string> traced_stages_; // ensure each stage only logged once

    // first-time lifecycle flags
    std::atomic<bool> first_handshake_started_{false};
    std::atomic<bool> first_handshake_completed_{false};
    std::atomic<bool> first_rma_post_attempt_{false};
    std::atomic<bool> first_rma_post_success_{false};
    std::atomic<bool> first_rma_post_eagain_{false};
    std::atomic<bool> first_rma_completion_{false};

    std::string localAddr_;
    mutable std::map<std::string, std::string> remoteAddrs_;
    mutable std::map<std::string, fi_addr_t> avAddrs_;
    mutable std::mutex avLock_;

    // Handshake support
    static constexpr size_t WAKEUP_MSG_SIZE = 1;
    static constexpr uint64_t recv_context_ = 0xDEADBEEF;
    mutable std::array<uint8_t, WAKEUP_MSG_SIZE> persistent_recv_buf_;
    mutable std::set<std::string> handshake_completed_;

    // utility to decode and print address info
    void logAddressInfo(const std::string& prefix, const void* addr_data, size_t addr_len, int addr_format = -1) const;

    // Handshake methods
    nixl_status_t postPersistentRecv() const;
    nixl_status_t performHandshake(const std::string &remote_agent, fi_addr_t remote_addr) const;

    // instrumentation counters
    mutable std::atomic<uint64_t> rma_posted_{0};
    mutable std::atomic<uint64_t> rma_completed_{0};
    mutable std::atomic<uint64_t> rma_eagain_{0};
        void logRmaStats(const char* tag) const;
         // helper to read and log any cq error entries when persistent -FI_EAGAIN occurs
        void dumpCQErrors(struct fid_cq* cq, const char* which) const;

    // tracing helper (idempotent per stage)
    void traceStage(const std::string &stage, const std::string &detail = "") const;
};

#endif
