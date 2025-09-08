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
#include <rdma/fi_eq.h>
#include <rdma/fi_ext.h>

#include <dlfcn.h>
#include "habanalabs/synapse_api.h"

#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <memory>

class nixlOfiMetadata : public nixlBackendMD {
public:
    fid_mr *mr;
    void *desc;

    nixlOfiMetadata() : nixlBackendMD(false), mr(nullptr), desc(nullptr) { }
    ~nixlOfiMetadata() { }
};

class nixlOfiRequest : public nixlBackendReqH {
public:
    size_t total_operations;
    size_t completed_operations;
    bool is_prepared;
    bool is_posted;
    
    // completion tracking
    fid_cq *cq;
    std::atomic<uint64_t> wr_id;
    
    // operation contexts for cleanup
    std::vector<std::unique_ptr<uint64_t>> op_contexts;
    
    // transfer parameters for delayed execution
    nixl_xfer_op_t operation;
    std::vector<nixlMetaDesc> local_descs;
    std::vector<nixlMetaDesc> remote_descs;
    std::string remote_agent;
    
    nixlOfiRequest() : total_operations(0), completed_operations(0), 
                       is_prepared(false), is_posted(false), cq(nullptr), wr_id(0) { }
    
    ~nixlOfiRequest() {
        // auto cleanup via unique_ptr
    }
    
    // helper methods
    bool isComplete() const { return completed_operations >= total_operations; }
    size_t remainingOperations() const { return total_operations - completed_operations; }
    
private:
    // disable copy
    nixlOfiRequest(const nixlOfiRequest&) = delete;
    nixlOfiRequest& operator=(const nixlOfiRequest&) = delete;
};

class nixlOfiHmemManager {
public:
    nixlOfiHmemManager();
    ~nixlOfiHmemManager();
    
    void initializeHmemCapabilities();
    void detectProviderCapabilities(struct fi_info* fi_info, const std::string& provider_name);
    fi_hmem_iface selectHmemInterface(const nixlBlobDesc &mem, uint64_t &device_id) const;
    
    nixl_status_t registerVramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta, 
                                     const struct fi_info* fi_info, fid_domain *domain) const;
    nixl_status_t registerSynapseAIMemoryExplicit(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta,
                                                  const struct fi_info* fi_info, fid_domain *domain) const;
    
    bool isZeSupported() const { return hmemZeSupported_; }
    bool isCudaSupported() const { return hmemCudaSupported_; }
    bool isSynapseaiSupported() const { return hmemSynapseaiSupported_; }

private:
    bool hmemZeSupported_;
    bool hmemCudaSupported_;
    bool hmemSynapseaiSupported_;
};

class nixlOfiEngine : public nixlBackendEngine {
public:
    // constructors and destructor
    nixlOfiEngine(const nixlBackendInitParams* init_params);
    ~nixlOfiEngine();

    // member functions
    bool supportsNotif() const override;
    bool supportsRemote() const override;
    bool supportsLocal() const override;
    bool supportsProgTh() const override;

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
                          
    nixl_status_t postPreparedXfer(nixlOfiRequest* ofi_req) const;

    nixl_status_t checkXfer(nixlBackendReqH* handle) const override;
    nixl_status_t releaseReqH(nixlBackendReqH* handle) const override;

    nixl_status_t getConnInfo(std::string &conn_info) const override;
    nixl_status_t loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) override;
    
    nixl_status_t getPublicData(const nixlBackendMD* meta, std::string &str) const override;
    nixl_status_t loadRemoteMD(const nixlBlobDesc &input, const nixl_mem_t &nixl_mem, 
                               const std::string &remote_agent, nixlBackendMD* &output) override;

    // Notification methods (required when supportsNotif() = true)
    nixl_status_t getNotifs(notif_list_t &notif_list) override;
    nixl_status_t genNotif(const std::string &remote_agent, const std::string &msg) const override;


private:
    // type definitions and nested classes

    // member functions
    void eqEventLoop();
    
    nixl_status_t setupEndpoint(bool connection_oriented);
    static nixl_status_t getEndpointAddress(fid_ep* endpoint, std::string& address);
    
    // parameter helpers
    void getStringParam(const nixlBackendInitParams* init_params, const std::string& key, std::string& value);
    void getLongParam(const nixlBackendInitParams* init_params, const std::string& key, long& value, long min_val, long max_val);
    void getSizeTParam(const nixlBackendInitParams* init_params, const std::string& key, size_t& value);
    
    // connection helpers
    nixl_status_t connectUnlocked(const std::string &remote_agent);
    
    // Memory registration helpers
    nixl_status_t registerDramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const;
    nixl_status_t registerVramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const;
    nixl_status_t registerSynapseAIMemoryExplicit(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const;

    // helper methods
    nixl_status_t handleCQError(fid_cq* cq, int error_ret) const;
    nixl_status_t driveProgress() const;  // reusable progress driving for FI_EAGAIN retry
    uint64_t getRemoteKey(nixlOfiMetadata* remote_meta) const;
    bool isConnectionEstablished(const std::string& remote_agent) const;
    nixl_status_t validateTransferParams(const nixlMetaDesc& local_desc, const nixlMetaDesc& remote_desc) const;
    

    // data members
    fid_fabric *fabric_;
    fid_domain *domain_;
    fid_ep *ep_;
    fid_cq *cq_;
    fid_cntr *txcntr_;
    fid_cntr *rxcntr_;
    fid_cntr *rma_cntr_;
    fid_eq *eq_;
    fid_pep *pep_;
    struct fi_info *fi_;
    struct fi_info *hints_;
    
    std::string providerName_;
    struct fi_info *cachedProviderInfo_;
    std::string localAddr_;
    mutable std::map<std::string, std::string> remoteAddrs_;
    mutable std::map<std::string, fid_ep *> connectedEps_;
    mutable std::map<std::string, fi_addr_t> avAddrs_;
    fid_av *av_;
    mutable std::mutex epLock_;
    bool isConnectionless_;

    std::thread eqThread_;
    std::atomic<bool> eqThreadStop_;
    std::atomic<bool> eqThreadPaused_;
    std::mutex eqPauseMutex_;
    std::condition_variable eqPauseCV_;
    long eqTimeoutMs_;
    std::string localAgentName_;
    
    std::unique_ptr<nixlOfiHmemManager> hmemManager_;
};

#endif
