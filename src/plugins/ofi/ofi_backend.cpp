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

#include "ofi_backend.h"
#include "common/nixl_log.h"
#include "serdes/serdes.h"

#include <optional>
#include <limits>
#include <string.h>
#include <unistd.h>

std::unique_ptr<nixlOfiEngine>
nixlOfiEngine::create(const nixlBackendInitParams &init_params) {
    auto engine = std::unique_ptr<nixlOfiEngine>(new nixlOfiEngine(init_params));
    if (engine->getInitErr()) {
        return nullptr;
    }
    return engine;
}

nixlOfiEngine::nixlOfiEngine(const nixlBackendInitParams &init_params)
    : nixlBackendEngine(&init_params) {
    // placeholder initialization
    NIXL_INFO << "OFI backend initialized";
}

nixlOfiEngine::~nixlOfiEngine() {
    // cleanup placeholder
}

nixl_mem_list_t nixlOfiEngine::getSupportedMems() const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    mems.push_back(VRAM_SEG);
    return mems;
}

nixl_status_t nixlOfiEngine::getConnInfo(std::string &str) const {
    str = workerAddr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::connect(const std::string &remote_agent) {
    if(remote_agent == localAgent) {
        return loadRemoteConnInfo(remote_agent, workerAddr);
    }

    return (remoteConnMap.find(remote_agent) == remoteConnMap.end()) ? NIXL_ERR_NOT_FOUND :
                                                                       NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::disconnect(const std::string &remote_agent) {
    auto search = remoteConnMap.find(remote_agent);

    if (search == remoteConnMap.end()) {
        return NIXL_ERR_NOT_FOUND;
    }

    remoteConnMap.erase(search);
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteConnInfo(const std::string &remote_agent,
                                                const std::string &remote_conn_info) {
    if(remoteConnMap.count(remote_agent)) {
        return NIXL_ERR_INVALID_PARAM;
    }

    // placeholder connection setup
    auto conn = std::make_shared<nixlOfiConnection>();
    conn->remoteAgent = remote_agent;
    remoteConnMap.insert({remote_agent, conn});

    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::registerMem(const nixlBlobDesc &mem,
                                         const nixl_mem_t &nixl_mem,
                                         nixlBackendMD* &out) {
    auto priv = std::make_unique<nixlOfiPrivateMetadata>();

    // placeholder memory registration

    out = priv.release();
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::deregisterMem(nixlBackendMD* meta) {
    nixlOfiPrivateMetadata *priv = (nixlOfiPrivateMetadata*) meta;
    // placeholder deregistration
    delete priv;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::getPublicData(const nixlBackendMD* meta,
                                           std::string &str) const {
    const nixlOfiPrivateMetadata *priv = (nixlOfiPrivateMetadata*) meta;
    str = priv->get();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlOfiEngine::loadLocalMD(nixlBackendMD* input,
                           nixlBackendMD* &output) {
    // placeholder implementation
    output = input;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::loadRemoteMD(const nixlBlobDesc &input,
                                          const nixl_mem_t &nixl_mem,
                                          const std::string &remote_agent,
                                          nixlBackendMD* &output) {
    // placeholder implementation
    return NIXL_ERR_NOT_SUPPORTED;
}

nixl_status_t nixlOfiEngine::unloadMD(nixlBackendMD* input) {
    // placeholder implementation
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::prepXfer(const nixl_xfer_op_t &operation,
                                      const nixl_meta_dlist_t &local,
                                      const nixl_meta_dlist_t &remote,
                                      const std::string &remote_agent,
                                      nixlBackendReqH* &handle,
                                      const nixl_opt_b_args_t* opt_args) const {
    // placeholder implementation
    handle = nullptr;
    return NIXL_ERR_NOT_SUPPORTED;
}

nixl_status_t nixlOfiEngine::estimateXferCost(const nixl_xfer_op_t &operation,
                                              const nixl_meta_dlist_t &local,
                                              const nixl_meta_dlist_t &remote,
                                              const std::string &remote_agent,
                                              nixlBackendReqH* const &handle,
                                              std::chrono::microseconds &duration,
                                              std::chrono::microseconds &err_margin,
                                              nixl_cost_t &method,
                                              const nixl_opt_args_t* opt_args) const {
    duration = std::chrono::microseconds(0);
    err_margin = std::chrono::microseconds(0);
    method = nixl_cost_t::ANALYTICAL_BACKEND;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlOfiEngine::postXfer(const nixl_xfer_op_t &operation,
                        const nixl_meta_dlist_t &local,
                        const nixl_meta_dlist_t &remote,
                        const std::string &remote_agent,
                        nixlBackendReqH *&handle,
                        const nixl_opt_b_args_t *opt_args) const {
    // placeholder implementation
    return NIXL_ERR_NOT_SUPPORTED;
}

nixl_status_t nixlOfiEngine::checkXfer(nixlBackendReqH* handle) const {
    // placeholder implementation
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::releaseReqH(nixlBackendReqH* handle) const {
    // placeholder implementation
    return NIXL_SUCCESS;
}

nixl_status_t
nixlOfiEngine::createGpuXferReq(const nixlBackendReqH &handle,
                                nixlGpuXferReqH &gpu_req_hndl) const {
    return NIXL_ERR_NOT_SUPPORTED;
}

void
nixlOfiEngine::releaseGpuXferReq(nixlGpuXferReqH gpu_req_hndl) const {}

nixl_status_t nixlOfiEngine::getNotifs(notif_list_t &notif_list) {
    if (!notif_list.empty()) return NIXL_ERR_INVALID_PARAM;

    // placeholder notification handling
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    // placeholder notification generation
    return NIXL_ERR_NOT_SUPPORTED;
}

nixl_status_t nixlOfiEngine::queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const {
    // placeholder memory query implementation
    return NIXL_ERR_NOT_SUPPORTED;
}

ofi_connection_ptr_t
nixlOfiEngine::getConnection(const std::string &remote_agent) const {
    auto search = remoteConnMap.find(remote_agent);
    return (search != remoteConnMap.end()) ? search->second : nullptr;
}

void
nixlOfiEngine::appendNotif(std::string remote_name, std::string msg) {
    notifMainList.emplace_back(std::move(remote_name), std::move(msg));
}