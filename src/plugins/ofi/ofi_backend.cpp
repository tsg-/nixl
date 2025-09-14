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
    // initialize fabric resources
    nixl_status_t status = nixlOfiUtils::setupFabric(getCustomParams());
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "failed to setup OFI fabric";
        initErr = true;
        return;
    }
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
    if (!nixlOfiUtils::ep || !nixlOfiUtils::fi) {
        NIXL_ERROR << "OFI fabric not initialized";
        return NIXL_ERR_BACKEND;
    }

    // get local endpoint address
    size_t addrlen = 0;
    int ret = fi_getname(&nixlOfiUtils::ep->fid, nullptr, &addrlen);
    if (ret != -FI_ETOOSMALL) {
        NIXL_ERROR << "fi_getname failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    std::vector<char> addr_buf(addrlen);
    ret = fi_getname(&nixlOfiUtils::ep->fid, addr_buf.data(), &addrlen);
    if (ret) {
        NIXL_ERROR << "fi_getname failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // convert address to string (provider-specific format)
    str = std::string(addr_buf.data(), addrlen);
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

    if (!nixlOfiUtils::av) {
        NIXL_ERROR << "address vector not initialized";
        return NIXL_ERR_BACKEND;
    }

    // create connection object
    auto conn = std::make_shared<nixlOfiConnection>();
    conn->remoteAgent = remote_agent;

    // insert remote address into address vector
    fi_addr_t fi_addr;
    int ret = fi_av_insert(nixlOfiUtils::av, remote_conn_info.data(), 1, &fi_addr, 0, nullptr);
    if (ret != 1) {
        NIXL_ERROR << "fi_av_insert failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // store the address for future use (would add fi_addr to connection object)
    remoteConnMap.insert({remote_agent, conn});

    NIXL_DEBUG << "loaded connection info for agent: " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::registerMem(const nixlBlobDesc &mem,
                                         const nixl_mem_t &nixl_mem,
                                         nixlBackendMD* &out) {
    auto priv = std::make_unique<nixlOfiPrivateMetadata>();

    // store memory info
    priv->addr = (void*)mem.addr;
    priv->length = mem.len;
    priv->mem_type = nixl_mem;

    // prepare memory region attributes
    struct fi_mr_attr attr = {0};
    struct iovec iov = {0};

    iov.iov_base = priv->addr;
    iov.iov_len = priv->length;

    attr.mr_iov = &iov;
    attr.iov_count = 1;
    attr.access = FI_SEND | FI_RECV | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    attr.offset = 0;
    attr.requested_key = FI_KEY_NOTAVAIL;  // let provider choose key
    attr.context = nullptr;
    attr.iface = FI_HMEM_SYSTEM;  // default to system memory

    // register memory region
    int ret = fi_mr_regattr(nixlOfiUtils::domain, &attr, 0, &priv->mr);
    if (ret) {
        NIXL_ERROR << "fi_mr_regattr failed: " << fi_strerror(-ret)
                   << " for memory " << priv->addr << " size " << priv->length;
        return NIXL_ERR_BACKEND;
    }

    // get memory region descriptor and key
    priv->mr_desc = fi_mr_desc(priv->mr);
    priv->mr_key = fi_mr_key(priv->mr);

    // create serialized key string for remote access
    priv->keyStr = std::to_string(priv->mr_key);

    NIXL_DEBUG << "registered memory: addr=" << priv->addr
               << " len=" << priv->length
               << " key=" << priv->mr_key;

    out = priv.release();
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiEngine::deregisterMem(nixlBackendMD* meta) {
    nixlOfiPrivateMetadata *priv = (nixlOfiPrivateMetadata*) meta;

    if (!priv) {
        NIXL_WARN << "attempted to deregister null metadata";
        return NIXL_ERR_INVALID_PARAM;
    }

    // deregister memory region
    if (priv->mr) {
        int ret = fi_close(&priv->mr->fid);
        if (ret) {
            NIXL_WARN << "fi_close for MR failed: " << fi_strerror(-ret);
        }

        NIXL_DEBUG << "deregistered memory: addr=" << priv->addr
                   << " len=" << priv->length
                   << " key=" << priv->mr_key;
    }

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