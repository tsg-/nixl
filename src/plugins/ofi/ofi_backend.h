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
#ifndef NIXL_SRC_PLUGINS_OFI_OFI_BACKEND_H
#define NIXL_SRC_PLUGINS_OFI_OFI_BACKEND_H

#include <vector>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <unordered_map>

#include "nixl.h"
#include "backend/backend_engine.h"
#include "common/str_tools.h"
#include "common/nixl_time.h"
#include "ofi_utils.h"

class nixlOfiConnection : public nixlBackendConnMD {
    private:
        std::string remoteAgent;
        fi_addr_t fi_addr;
        bool handshake_complete;

    public:
        nixlOfiConnection() : fi_addr(FI_ADDR_UNSPEC), handshake_complete(false) {}

        [[nodiscard]] fi_addr_t getFiAddr() const noexcept {
            return fi_addr;
        }

        [[nodiscard]] bool isHandshakeComplete() const noexcept {
            return handshake_complete;
        }

    friend class nixlOfiEngine;
};

using ofi_connection_ptr_t = std::shared_ptr<nixlOfiConnection>;

class nixlOfiPrivateMetadata : public nixlBackendMD {
    private:
        nixl_blob_t keyStr;
        struct fid_mr *mr;
        void *mr_desc;
        uint64_t mr_key;
        void *addr;
        size_t length;
        nixl_mem_t mem_type;

    public:
        nixlOfiPrivateMetadata() : nixlBackendMD(true), mr(nullptr), mr_desc(nullptr),
                                   mr_key(0), addr(nullptr), length(0), mem_type(DRAM_SEG) {
        }

        [[nodiscard]] const std::string& get() const noexcept {
            return keyStr;
        }

        [[nodiscard]] struct fid_mr* getMr() const noexcept {
            return mr;
        }

        [[nodiscard]] void* getMrDesc() const noexcept {
            return mr_desc;
        }

        [[nodiscard]] uint64_t getMrKey() const noexcept {
            return mr_key;
        }

        [[nodiscard]] void* getAddr() const noexcept {
            return addr;
        }

        [[nodiscard]] size_t getLength() const noexcept {
            return length;
        }

    friend class nixlOfiEngine;
};

class nixlOfiPublicMetadata : public nixlBackendMD {
public:
    nixlOfiPublicMetadata() : nixlBackendMD(false), remote_key(0) {}

    ofi_connection_ptr_t conn;
    uint64_t remote_key;  // store the parsed remote memory key

    [[nodiscard]] uint64_t getRemoteKey() const noexcept {
        return remote_key;
    }

private:
};

class nixlOfiEngine : public nixlBackendEngine {
public:
    static std::unique_ptr<nixlOfiEngine>
    create(const nixlBackendInitParams &init_params);

    // Plugin framework expects a constructor taking a pointer
    explicit nixlOfiEngine(const nixlBackendInitParams *init_params_ptr)
        : nixlOfiEngine(*init_params_ptr) {}

    ~nixlOfiEngine();

    bool
    supportsRemote() const override {
        return true;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    bool
    supportsNotif() const override {
        return true;
    }

    nixl_mem_list_t
    getSupportedMems() const override;

    /* object management */
    nixl_status_t
    getPublicData(const nixlBackendMD *meta, std::string &str) const override;
    nixl_status_t
    getConnInfo(std::string &str) const override;
    nixl_status_t
    loadRemoteConnInfo(const std::string &remote_agent,
                       const std::string &remote_conn_info) override;

    nixl_status_t
    connect(const std::string &remote_agent) override;
    nixl_status_t
    disconnect(const std::string &remote_agent) override;

    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;
    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override;

    nixl_status_t
    loadRemoteMD(const nixlBlobDesc &input,
                 const nixl_mem_t &nixl_mem,
                 const std::string &remote_agent,
                 nixlBackendMD *&output) override;
    nixl_status_t
    unloadMD(nixlBackendMD *input) override;

    // data transfer
    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    estimateXferCost(const nixl_xfer_op_t &operation,
                     const nixl_meta_dlist_t &local,
                     const nixl_meta_dlist_t &remote,
                     const std::string &remote_agent,
                     nixlBackendReqH *const &handle,
                     std::chrono::microseconds &duration,
                     std::chrono::microseconds &err_margin,
                     nixl_cost_t &method,
                     const nixl_opt_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;
    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

    nixl_status_t
    createGpuXferReq(const nixlBackendReqH &handle, nixlGpuXferReqH &gpu_req_hndl) const override;

    void
    releaseGpuXferReq(nixlGpuXferReqH gpu_req_hndl) const override;

    nixl_status_t
    getNotifs(notif_list_t &notif_list) override;
    nixl_status_t
    genNotif(const std::string &remote_agent, const std::string &msg) const override;

    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const override;

protected:
    nixlOfiEngine(const nixlBackendInitParams &init_params);

private:
    // store our local endpoint connection info (equivalent to UCX workerAddr)
    std::string localConnInfo;

    /* notifications */
    notif_list_t notifMainList;

    // map of agent name to connection info
    std::unordered_map<std::string, ofi_connection_ptr_t, std::hash<std::string>, strEqual>
        remoteConnMap;

    ofi_connection_ptr_t
    getConnection(const std::string &remote_agent) const;

    void
    appendNotif(std::string remote_name, std::string msg);
};

class nixlOfiBackendReqH : public nixlBackendReqH {
public:
    struct fi_context context;    // must be zeroed before use
    fi_addr_t remote_fi_addr;     // provider address of remote EP
    uint64_t remote_key;          // remote MR key
    void *remote_addr;            // remote buffer address
    size_t transfer_size;         // size of transfer actually posted
    nixl_xfer_op_t operation;     // NIXL_READ / NIXL_WRITE
    bool completed;               // completion flag

    nixlOfiBackendReqH()
        : nixlBackendReqH(),
          remote_fi_addr(FI_ADDR_UNSPEC),
          remote_key(0),
          remote_addr(nullptr),
          transfer_size(0),
          operation(NIXL_READ),
          completed(false) {
        memset(&context, 0, sizeof(context));
    }

    virtual ~nixlOfiBackendReqH() {}
};

#endif