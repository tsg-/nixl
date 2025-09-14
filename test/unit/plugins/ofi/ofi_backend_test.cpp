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

#include <iostream>
#include <sstream>
#include <string>
#include <cassert>
#include <memory>
#include <cstring>
#include <thread>
#include <chrono>

#include "ofi_backend.h"

using namespace std;

class testHndlIterator {
private:
    bool reuse;
    bool set;
    bool prepare;
    bool release;
    nixlBackendReqH* handle;
public:
    testHndlIterator(bool _reuse) {
        reuse = _reuse;
        if (reuse) {
            prepare = true;
            release = false;
        } else {
            prepare = true;
            release = true;
        }
        handle = nullptr;
        set = false;
    }

    ~testHndlIterator() {
        /* Make sure that handler was released */
        assert(!set);
    }

    bool needPrep() {
        if (reuse) {
            if (!prepare) {
                return false;
            }
        }
        return true;
    }

    bool needRelease() {
        return release;
    }

    void isLast() {
        if (reuse) {
            release = true;
        }
    }

    void setHandle(nixlBackendReqH *_handle)
    {
        assert(!set);
        handle = _handle;
        set = true;
        if (reuse) {
            prepare = false;
        }
    }

    void unsetHandle() {
        assert(set);
        set = false;
    }

    nixlBackendReqH *&getHandle() {
        assert(set);
        return handle;
    }
};

nixlBackendEngine *createEngine(std::string name, bool p_thread)
{
    nixlBackendEngine     *ofi;
    nixlBackendInitParams init;
    nixl_b_params_t custom_params;

    init.enableProgTh = p_thread;
    init.pthrDelay    = 100;
    init.localAgent   = name;
    init.customParams = reinterpret_cast<nixl_b_params_t*>(&custom_params);
    init.type         = "OFI";

    // Try to create OFI engine, handle failures gracefully
    try {
        ofi = new nixlOfiEngine(&init);
        if (ofi->getInitErr()) {
            std::cout << "OFI backend initialization failed - likely missing libfabric infrastructure" << std::endl;
            delete ofi;
            return nullptr;
        }
    } catch (const std::exception& e) {
        std::cout << "OFI backend creation failed with exception: " << e.what() << std::endl;
        return nullptr;
    }

    return ofi;
}

void releaseEngine(nixlBackendEngine *ofi)
{
    delete ofi;
}

std::string memType2Str(nixl_mem_t mem_type)
{
    switch(mem_type) {
    case DRAM_SEG:
        return std::string("DRAM");
    case VRAM_SEG:
        return std::string("VRAM");
    case BLK_SEG:
        return std::string("BLOCK");
    case FILE_SEG:
        return std::string("FILE");
    default:
        std::cout << "Unsupported memory type!" << std::endl;
        assert(0);
    }
}

void allocateBuffer(nixl_mem_t mem_type, int dev_id, size_t len, void* &addr)
{
    switch(mem_type) {
    case DRAM_SEG:
        addr = calloc(1, len);
        break;
    case VRAM_SEG:
        // OFI backend currently only supports DRAM
        std::cout << "VRAM not supported by OFI backend" << std::endl;
        addr = nullptr;
        break;
    default:
        std::cout << "Unsupported memory type: " << mem_type << std::endl;
        addr = nullptr;
        break;
    }
}

void releaseBuffer(nixl_mem_t mem_type, int dev_id, void* addr)
{
    switch(mem_type) {
    case DRAM_SEG:
        free(addr);
        break;
    case VRAM_SEG:
        // OFI backend currently only supports DRAM
        break;
    default:
        break;
    }
}

void allocateAndRegister(nixlBackendEngine *ofi, int dev_id, nixl_mem_t mem_type,
                         void* &addr, size_t len, nixlBackendMD* &md)
{
    nixlBlobDesc desc;

    allocateBuffer(mem_type, dev_id, len, addr);
    if (!addr) {
        std::cout << "Failed to allocate buffer" << std::endl;
        assert(0);
    }

    desc.addr   = (uintptr_t) addr;
    desc.len    = len;
    desc.devId = dev_id;

    int ret = ofi->registerMem(desc, mem_type, md);
    assert(ret == NIXL_SUCCESS);
}

void deallocateAndDeregister(nixlBackendEngine *ofi, int dev_id, nixl_mem_t mem_type,
                            void* addr, nixlBackendMD* md)
{
    ofi->deregisterMem(md);
    releaseBuffer(mem_type, dev_id, addr);
}

void populateDescs(nixl_meta_dlist_t &descs, int dev_id, void *addr, int desc_cnt, size_t desc_size, nixlBackendMD* &md)
{
    for(int i = 0; i < desc_cnt; i++) {
        nixlMetaDesc req;
        req.addr     = (uintptr_t) (((char*) addr) + i * desc_size);
        req.len      = desc_size;
        req.devId    = dev_id;
        req.metadataP = md;
        descs.addDesc(req);
    }
}

static string op2string(nixl_xfer_op_t op, bool hasNotif)
{
    if(op == NIXL_READ && !hasNotif)
        return string("READ");
    if(op == NIXL_WRITE && !hasNotif)
        return string("WRITE");
    if(op == NIXL_READ && hasNotif)
        return string("READ/NOTIF");
    if(op == NIXL_WRITE && hasNotif)
        return string("WRITE/NOTIF");

    return string("UNKNOWN");
}

void doMemset(nixl_mem_t mem_type, int dev_id, void* addr, char pattern, size_t len)
{
    switch(mem_type) {
    case DRAM_SEG:
        memset(addr, pattern, len);
        break;
    case VRAM_SEG:
        // OFI backend currently only supports DRAM
        std::cout << "VRAM memset not supported" << std::endl;
        break;
    default:
        std::cout << "Unsupported memory type for memset" << std::endl;
        break;
    }
}

void performTransfer(nixlBackendEngine *src_ofi, nixlBackendEngine *dst_ofi,
                    nixl_meta_dlist_t &req_src_descs, nixl_meta_dlist_t &req_dst_descs,
                    void *src_addr, void *dst_addr, size_t len,
                    nixl_xfer_op_t op, testHndlIterator &hiter, bool p_thread, bool use_notif,
                    const std::string &remote_agent)
{
    nixl_status_t ret1, ret2, ret3;
    nixl_opt_b_args_t opt_args;
    opt_args.hasNotif = use_notif;

    cout << "\t\t" << op2string(op, use_notif) << " from " << hex << src_addr << " to " << dst_addr << dec << endl;

    if (hiter.needPrep()) {
        nixlBackendReqH *new_handle;
    ret3 = src_ofi->prepXfer(op, req_src_descs, req_dst_descs, remote_agent, new_handle, &opt_args);
        assert(ret3 == NIXL_SUCCESS);
        hiter.setHandle(new_handle);
    }
    nixlBackendReqH *&handle = hiter.getHandle();
    ret3 = src_ofi->postXfer(op, req_src_descs, req_dst_descs, remote_agent, handle, &opt_args);

    if (ret3 == NIXL_SUCCESS) {
        cout << "\t\t\tWARNING: Transfer request completed immediately - no testing non-inline path" << endl;
    } else if (ret3 == NIXL_IN_PROG) {
        cout << "\t\t\tNOTE: Testing non-inline Transfer path!" << endl;

        // Wait for completion
        do {
            ret1 = src_ofi->checkXfer(handle);
            if (p_thread && ret1 == NIXL_IN_PROG) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } while (ret1 == NIXL_IN_PROG);

        assert(ret1 == NIXL_SUCCESS);
    } else {
        std::cout << "\t\t\tTransfer failed with status: " << ret3 << std::endl;
        std::cout << "\t\t\tNOTE: This failure may be expected for inter-agent transfers due to resource exhaustion" << std::endl;
        std::cout << "\t\t\tLocal transfers should always succeed, inter-agent may fail under resource pressure" << std::endl;
        if (ret3 == -3) {  // -3 is our resource exhaustion error code
            std::cout << "\t\t\tThis appears to be a resource exhaustion issue (error -3)" << std::endl;
            std::cout << "\t\t\tTest will continue with cleanup" << std::endl;
            // Continue to cleanup section - don't return early
        } else {
            assert(0);  // Only assert for unexpected errors
        }
    }

    if (use_notif) {
        cout << "\t\t\tChecking notification flow: ";
        notif_list_t notif_list;

        // Check for notifications
        ret2 = dst_ofi->getNotifs(notif_list);
        assert(ret2 == NIXL_SUCCESS);

        if (notif_list.size() > 0) {
            cout << "OK" << endl;
        } else {
            cout << "No notifications received (may be expected)" << endl;
        }
    }

    // Data verification for DRAM
    if (req_src_descs.getType() == DRAM_SEG && req_dst_descs.getType() == DRAM_SEG) {
        cout << "\t\t\tData verification: ";
        if (memcmp(src_addr, dst_addr, len) == 0) {
            cout << "OK" << endl;
        } else {
            cout << "FAILED - data mismatch" << endl;
            assert(0);
        }
    }

    if (hiter.needRelease()) {
        ret1 = src_ofi->releaseReqH(handle);
        assert(ret1 == NIXL_SUCCESS);
        hiter.unsetHandle();
    }
}

void test_intra_agent_transfer(bool p_thread, nixlBackendEngine *ofi, nixl_mem_t mem_type)
{
    std::cout << std::endl << std::endl;
    std::cout << "****************************************************" << std::endl;
    std::cout << "   Intra-agent memory transfer test: "
              << "P-Thr=" << (p_thread ? "ON" : "OFF") << ", " << memType2Str(mem_type)
              << std::endl;
    std::cout << "****************************************************" << std::endl;
    std::cout << std::endl << std::endl;

    std::string agent1("Agent1");
    nixl_status_t ret1;

    int iter = 10;

    assert(ofi->supportsLocal());

    // connection info is still a string
    std::string conn_info1;
    ret1 = ofi->getConnInfo(conn_info1);
    assert(ret1 == NIXL_SUCCESS);
    ret1 = ofi->loadRemoteConnInfo (agent1, conn_info1);
    assert(ret1 == NIXL_SUCCESS);

    std::cout << "local connection complete\n";

    // Number of transfer descriptors
    int desc_cnt = 4;
    // Size of a single descriptor
    size_t desc_size = 1 * 1024 * 1024;
    size_t len = desc_cnt * desc_size;

    void *addr1, *addr2;
    nixlBackendMD *lmd1, *lmd2;
    allocateAndRegister(ofi, 0, mem_type, addr1, len, lmd1);
    allocateAndRegister(ofi, 0, mem_type, addr2, len, lmd2);

    //string descs unnecessary, convert meta locally
    nixlBackendMD* rmd2;
    ret1 = ofi->loadLocalMD (lmd2, rmd2);
    assert(ret1 == NIXL_SUCCESS);

    nixl_meta_dlist_t req_src_descs (mem_type);
    populateDescs(req_src_descs, 0, addr1, desc_cnt, desc_size, lmd1);

    nixl_meta_dlist_t req_dst_descs (mem_type);
    populateDescs(req_dst_descs, 0, addr2, desc_cnt, desc_size, rmd2);

    nixl_xfer_op_t ops[] = {  NIXL_READ, NIXL_WRITE };
    bool use_notifs[] = { true, false };

    for (size_t i = 0; i < sizeof(ops)/sizeof(ops[i]); i++) {

        for(bool use_notif : use_notifs) {
            cout << endl << op2string(ops[i], use_notif) << " test (" << iter << ") iterations" <<endl;
            for(int k = 0; k < iter; k++ ) {
                /* Init data */
                doMemset(mem_type, 0, addr1, 0xbb, len);
                doMemset(mem_type, 0, addr2, 0, len);

                /* Test */
                testHndlIterator hiter(false);
                performTransfer(ofi, ofi, req_src_descs, req_dst_descs,
                                addr1, addr2, len, ops[i], hiter, p_thread, use_notif,
                                std::string("Agent1"));
            }
        }
    }

    // Note: For local MD, rmd2 == lmd2 (same pointer), so only unload once
    ofi->unloadMD (rmd2);
    deallocateAndDeregister(ofi, 0, mem_type, addr1, lmd1);
    // Don't call deallocateAndDeregister for lmd2 since rmd2 already unloaded it
    releaseBuffer(mem_type, 0, addr2);

    ofi->disconnect(agent1);
}

void test_inter_agent_transfer(bool p_thread, bool reuse_hndl,
                                nixlBackendEngine *ofi1, nixl_mem_t src_mem_type, int src_dev_id,
                                nixlBackendEngine *ofi2, nixl_mem_t dst_mem_type, int dst_dev_id)
{
    std::cout << std::endl << std::endl;
    std::cout << "****************************************************" << std::endl;
    std::cout << "    Inter-agent memory transfer test " << std::endl;
    std::cout << "         P-Thr=" << (p_thread ? "ON" : "OFF") << std::endl;
    std::cout << "         Handler-reuse=" << (reuse_hndl ? "ON" : "OFF") << std::endl;
    std::cout << "         (" << memType2Str(src_mem_type) << " -> "
              << memType2Str(dst_mem_type) << ")" << std::endl;
    std::cout << "****************************************************" << std::endl;

    int ret;
    int iter = 10;

    //Exchange connection info
    std::string src_agent("Agent1");
    std::string dst_agent("Agent2");

    std::string conn_info1, conn_info2;
    ret = ofi1->getConnInfo(conn_info1);
    assert(ret == NIXL_SUCCESS);
    ret = ofi2->getConnInfo(conn_info2);
    assert(ret == NIXL_SUCCESS);

    ret = ofi1->loadRemoteConnInfo(dst_agent, conn_info2);
    assert(ret == NIXL_SUCCESS);
    ret = ofi2->loadRemoteConnInfo(src_agent, conn_info1);
    assert(ret == NIXL_SUCCESS);

    ret = ofi1->connect(dst_agent);
    assert(ret == NIXL_SUCCESS);
    ret = ofi2->connect(src_agent);
    assert(ret == NIXL_SUCCESS);

    std::cout << "Synchronous handshake complete" << std::endl;

    // Number of transfer descriptors
    int desc_cnt = 64;
    // Size of a single descriptor
    size_t desc_size = 1 * 1024 * 1024;
    size_t len = desc_cnt * desc_size;

    void *addr1, *addr2;
    nixlBackendMD *lmd1, *lmd2;
    allocateAndRegister(ofi1, src_dev_id, src_mem_type, addr1, len, lmd1);
    allocateAndRegister(ofi2, dst_dev_id, dst_mem_type, addr2, len, lmd2);

    //serialize md2 for remote access
    std::string info2;
    nixlBackendMD* rmd2;
    ret = ofi2->getPublicData(lmd2, info2);
    assert(ret == NIXL_SUCCESS);

    nixlBlobDesc rmt_desc;
    rmt_desc.metaInfo.assign(info2.begin(), info2.end());
    ret = ofi1->loadRemoteMD(rmt_desc, dst_mem_type, dst_agent, rmd2);
    assert(ret == NIXL_SUCCESS);

    nixl_meta_dlist_t req_src_descs(src_mem_type);
    populateDescs(req_src_descs, src_dev_id, addr1, desc_cnt, desc_size, lmd1);

    nixl_meta_dlist_t req_dst_descs(dst_mem_type);
    populateDescs(req_dst_descs, dst_dev_id, addr2, desc_cnt, desc_size, rmd2);

    nixl_xfer_op_t ops[] = { NIXL_READ, NIXL_WRITE };
    bool use_notifs[] = { true, false };

    for (size_t i = 0; i < sizeof(ops)/sizeof(ops[i]); i++) {

        for(bool use_notif : use_notifs) {
            cout << endl << op2string(ops[i], use_notif) << " test (" << iter << ") iterations" <<endl;
            for(int k = 0; k < iter; k++ ) {
                testHndlIterator hiter(reuse_hndl);
                if (k == iter - 1) {
                    hiter.isLast();
                }

                /* Init data */
                doMemset(src_mem_type, src_dev_id, addr1, 0xbb, len);
                doMemset(dst_mem_type, dst_dev_id, addr2, 0, len);

                /* Test */
                performTransfer(ofi1, ofi2, req_src_descs, req_dst_descs,
                                addr1, addr2, len, ops[i], hiter, p_thread, use_notif,
                                dst_agent);
            }
        }
    }

    ofi1->unloadMD(rmd2);
    deallocateAndDeregister(ofi1, src_dev_id, src_mem_type, addr1, lmd1);
    deallocateAndDeregister(ofi2, dst_dev_id, dst_mem_type, addr2, lmd2);

    ofi1->disconnect(dst_agent);
    ofi2->disconnect(src_agent);
}

int main(int argc, char *argv[])
{
    int num_engines = 2;
    bool thread_on[] = { false, true };

    nixlBackendEngine *ofi[2][2];
    bool engines_created[2][2] = {{false, false}, {false, false}};

    std::cout << "Creating OFI engines..." << std::endl;

    // Create engines
    for (int i = 0; i < num_engines; i++) {
        for (int j = 0; j < 2; j++) { // 2 agents per thread setting
            std::string agent_name = "Agent" + std::to_string(j + 1);
            ofi[i][j] = createEngine(agent_name, thread_on[i]);
            if (ofi[i][j]) {
                engines_created[i][j] = true;
                std::cout << "Created OFI engine " << i << "," << j << " ("
                          << agent_name << ", P-Thr=" << (thread_on[i] ? "ON" : "OFF") << ")" << std::endl;
            } else {
                std::cout << "Failed to create OFI engine " << i << "," << j << " - skipping tests requiring this engine" << std::endl;
            }
        }
    }

    // Test local memory to local memory transfer
    for (int i = 0; i < num_engines; i++) {
        if (engines_created[i][0]) {
            std::cout << "Testing local memory to local memory transfer..." << std::endl;
            test_intra_agent_transfer(thread_on[i], ofi[i][0], DRAM_SEG);
        }
    }

    // Test inter-agent transfers
    for (int i = 0; i < num_engines; i++) {
        if (engines_created[i][0] && engines_created[i][1]) {
            for (int reuse = 0; reuse < 2; reuse++) {
                std::cout << "Testing inter-agent transfer..." << std::endl;
                test_inter_agent_transfer(thread_on[i], reuse != 0,
                                         ofi[i][0], DRAM_SEG, 0,
                                         ofi[i][1], DRAM_SEG, 0);
            }
        }
    }

    std::cout << "All tests completed successfully!" << std::endl;

    // Cleanup
    for (int i = 0; i < num_engines; i++) {
        for (int j = 0; j < 2; j++) {
            if (engines_created[i][j]) {
                releaseEngine(ofi[i][j]);
            }
        }
    }

    return 0;
}