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
// #include <rdma/fi_cq.h>
#include <rdma/fi_ext.h>

#include <dlfcn.h>
#include "habanalabs/synapse_api.h"

#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <unordered_set>

class nixlOfiMetadata : public nixlBackendMD {
public:
    fid_mr *mr;
    void *desc;

    nixlOfiMetadata() : nixlBackendMD(false), mr(nullptr), desc(nullptr) { }
    ~nixlOfiMetadata() { }
};

class nixlOfiRequest : public nixlBackendReqH {
public:
    fid_cq *cq;
    std::atomic<uint64_t> wr_id;  // CRITICAL FIX: Atomic completion tracking

    nixlOfiRequest() : cq(nullptr), wr_id(0) { }
    ~nixlOfiRequest() { }
};


// max xfer ids per notification
#define OFI_MAX_XFER_IDS 128

// binary notification structure for ofi plugin
struct OfiBinaryNotification {
    char agent_name[256];        // fixed-size agent name (null-terminated)
    char message[1024];         // fixed-size message (null-terminated)
    uint32_t xfer_id_count;     // number of xfer ids
    uint32_t xfer_ids[OFI_MAX_XFER_IDS]; // fixed array of xfer ids

    // clear all fields to zero
    void clear() {
        memset(this, 0, sizeof(OfiBinaryNotification));
    }

    // set agent name with bounds checking
    void setAgentName(const std::string &name) {
        strncpy(agent_name, name.c_str(), sizeof(agent_name) - 1);
        agent_name[sizeof(agent_name) - 1] = '\0';
    }

    // set message with bounds checking
    void setMessage(const std::string &msg) {
        strncpy(message, msg.c_str(), sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }

    // add xfer id if space available
    void addXferId(uint32_t xfer_id) {
        if (xfer_id_count < OFI_MAX_XFER_IDS) {
            xfer_ids[xfer_id_count++] = xfer_id;
        }
    }

    // get agent name as string
    std::string getAgentName() const {
        return std::string(agent_name);
    }

    // get message as string
    std::string getMessage() const {
        return std::string(message);
    }

    // get all xfer ids as unordered set
    std::unordered_set<uint32_t> getXferIds() const {
        std::unordered_set<uint32_t> result;
        for (uint32_t i = 0; i < xfer_id_count && i < OFI_MAX_XFER_IDS; ++i) {
            result.insert(xfer_ids[i]);
        }
        return result;
    }
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

    nixl_status_t checkXfer(nixlBackendReqH* handle) const override;
    nixl_status_t releaseReqH(nixlBackendReqH* handle) const override;

    nixl_status_t getConnInfo(std::string &conn_info) const override;
    nixl_status_t loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) override;
    
    nixl_status_t getPublicData(const nixlBackendMD* meta, std::string &str) const override;
    nixl_status_t loadRemoteMD(const nixlBlobDesc &input, const nixl_mem_t &nixl_mem, 
                               const std::string &remote_agent, nixlBackendMD* &output) override;

    // notification methods (required when supportsNotif() = true)
    nixl_status_t getNotifs(notif_list_t &notif_list) override;
    nixl_status_t genNotif(const std::string &remote_agent, const std::string &msg) const override;

private:
    // helper functions for releaseReqH
    void atomicDecrementCompletions(std::atomic<uint64_t>& counter, uint64_t count) const;
    bool handleErrorCompletion(fid_cq* cq, std::atomic<uint64_t>& pending_ops) const;
    int drainCompletionBatch(fid_cq* cq, std::atomic<uint64_t>& pending_ops) const;

    // notification system helper methods
    void processNotification(const std::string &serialized_notif);
    bool allXferIdsReceived(const std::unordered_set<uint32_t> &expected);
    void checkPendingNotifications();
    void addReceivedXferId(uint32_t xfer_id);

    // type definitions and nested classes
    struct ProviderConfig {
        std::string name;
        enum fi_ep_type ep_type;
        uint64_t caps;
        uint64_t mode;
        uint64_t mr_mode;
        fi_resource_mgmt resource_mgmt;
        struct fi_tx_attr tx_attr;
        struct fi_rx_attr rx_attr;
        uint32_t addr_format;
        enum fi_progress data_progress;
        enum fi_progress control_progress;
    };
    
    // static member variables
    static const ProviderConfig SUPPORTED_PROVIDERS[];
    static const size_t NUM_SUPPORTED_PROVIDERS;

    // member functions
    void eq_event_loop();
    void connectionProgressFunc();
    void driveProgress() const;
    void driveProgressIfNeeded() const;
    int ofi_progress_manual(fid_cq *cq) const;
    bool isConnectionlessProvider() const;

    // cq reference counting helpers
    fid_cq* acquireCQ() const;
    void releaseCQ(fid_cq* cq) const;
    nixl_status_t setupEndpoint(bool connection_oriented);
    static nixl_status_t getEndpointAddress(fid_ep* endpoint, std::string& address);
    static void detectHmemCapabilities(struct fi_info* fi_info,
                                       const std::string& provider_name,
                                       bool& cuda_supported,
                                       bool& ze_supported,
                                       bool& synapseai_supported);
    static const ProviderConfig* findProviderConfig(const std::string& provider_name);
    
    // parameter helpers
    void getStringParam(const nixlBackendInitParams* init_params, const std::string& key, std::string& value);
    void getLongParam(const nixlBackendInitParams* init_params, const std::string& key, long& value, long min_val, long max_val);
    void getSizeTParam(const nixlBackendInitParams* init_params, const std::string& key, size_t& value);
    
    // connection helpers
    nixl_status_t connect_unlocked(const std::string &remote_agent);
    
    void configureHintsForProvider(struct fi_info* hints, const std::string& provider_name);
    
    // Memory registration helpers
    static uint64_t getMemoryRegistrationAccessFlags(const struct fi_info* fi_info);
    fi_hmem_iface selectHmemInterface(const nixlBlobDesc &mem, uint64_t &device_id) const;
    nixl_status_t registerDramMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const;
    nixl_status_t registerHmemMemory(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta, fi_hmem_iface iface, uint64_t device_id) const;

    // data members
    fid_fabric *fabric_;
    fid_domain *domain_;
    fid_ep *ep_;
    fid_cq *cq_;
    fid_eq *eq_;
    fid_pep *pep_;
    struct fi_info *fi_;

    // cq reference counting to prevent destructor race
    mutable std::atomic<int> cq_refcount_;
    mutable std::mutex cq_mutex_;

    std::string providerName_;
    struct fi_info *cachedProviderInfo_;
    std::string localAddr_;
    mutable std::map<std::string, std::string> remoteAddrs_;
    mutable std::map<std::string, fid_ep *> connectedEps_;
    mutable std::map<std::string, fi_addr_t> shmAddrs_;
    fid_av *av_;
    mutable std::mutex epLock_;

    // notification system with thread safety
    mutable std::mutex notif_mutex_;
    std::vector<std::pair<std::string, std::string>> notifMainList_;

    // pending notification tracking
    struct PendingNotification {
        std::string remote_agent;
        std::string message;
        std::unordered_set<uint32_t> expected_xfer_ids;
        std::chrono::steady_clock::time_point received_time;

        PendingNotification(const std::string &agent,
                            const std::string &msg,
                            const std::unordered_set<uint32_t> &xfer_ids)
            : remote_agent(agent),
              message(msg),
              expected_xfer_ids(xfer_ids),
              received_time(std::chrono::steady_clock::now()) {}
    };

    mutable std::mutex receiver_tracking_mutex_;
    std::unordered_set<uint32_t> received_remote_writes_;
    std::vector<PendingNotification> pending_notifications_;

    bool isConnectionless_;

    std::thread eqThread_;
    std::atomic<bool> eqThreadStop_;
    std::atomic<bool> eqThreadPaused_;
    std::mutex eqPauseMutex_;
    std::condition_variable eqPauseCV_;
    long eqTimeoutMs_;

    // connection-focused progress thread
    std::thread connectionProgressThread_;
    std::atomic<bool> connectionProgressStop_;
    std::atomic<bool> shutdownFlag_;
    bool connectionProgressEnabled_;
    nixlTime::us_t connectionProgressDelay_;

    // main-thread progress rate limiting
    mutable std::atomic<std::chrono::steady_clock::time_point> lastProgressTime_;
    static const std::chrono::milliseconds PROGRESS_INTERVAL;
    bool hmemZeSupported_;
    bool hmemCudaSupported_;
    bool hmemSynapseaiSupported_;

    std::string localAgentName_;

    // synapseai dynamic loading handles
    static void *synapseai_handle_;
    static void *hlthunk_handle_;
    
    struct synapseai_ops {
        synStatus (*synInitialize)(void);
        synStatus (*synDestroy)(void);
        synStatus (*synDeviceAcquireByModuleId)(synDeviceId *pDeviceId, const synModuleId moduleId);
        synStatus (*synDeviceGetInfoV2)(const synDeviceId deviceId, synDeviceInfoV2 *pDeviceInfo);
        synStatus (*synStreamCreateGeneric)(synStreamHandle *pStreamHandle, const synDeviceId deviceId, const uint32_t flags);
        int (*hlthunk_device_mapped_memory_export_dmabuf_fd)(int fd, uint64_t addr, uint64_t size, uint64_t offset, uint32_t flags);
    };
    static synapseai_ops synapseai_ops_;
    
    nixl_status_t registerSynapseAIMemoryExplicit(const nixlBlobDesc &mem, nixlOfiMetadata *ofi_meta) const;
};

#endif
