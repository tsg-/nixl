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
        // placeholder for ofi endpoint handles

    public:
        // placeholder getters

    friend class nixlOfiEngine;
};

using ofi_connection_ptr_t = std::shared_ptr<nixlOfiConnection>;

class nixlOfiPrivateMetadata : public nixlBackendMD {
    private:
        // placeholder for ofi memory registration
        nixl_blob_t keyStr;

    public:
        nixlOfiPrivateMetadata() : nixlBackendMD(true) {
        }

        [[nodiscard]] const std::string& get() const noexcept {
            return keyStr;
        }

    friend class nixlOfiEngine;
};

class nixlOfiPublicMetadata : public nixlBackendMD {
public:
    nixlOfiPublicMetadata() : nixlBackendMD(false) {}

    // placeholder for remote key handling

    ofi_connection_ptr_t conn;

private:
    // placeholder for remote key storage
};

class nixlOfiEngine : public nixlBackendEngine {
public:
    static std::unique_ptr<nixlOfiEngine>
    create(const nixlBackendInitParams &init_params);

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
    // placeholder for ofi fabric and domain handles
    std::string workerAddr;

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

#endif