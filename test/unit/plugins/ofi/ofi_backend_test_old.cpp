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

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "ofi_backend.h"
#include "common/nixl_log.h"

class OfiBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_params.localAgent = "test_agent";
        custom_params["provider"] = "verbs";
        custom_params["eq_timeout_ms"] = "100";
        init_params.customParams = reinterpret_cast<nixl_b_params_t*>(&custom_params);
    }

    void TearDown() override {
    }

    // Helper function to safely create OFI engine for testing
    std::unique_ptr<nixlOfiEngine> createTestEngine() {
        std::unique_ptr<nixlOfiEngine> engine;
        try {
            engine.reset(new nixlOfiEngine(&init_params));
        } catch (const std::exception& e) {
            // Return null - calling test should skip
            return nullptr;
        }

        if (engine && engine->getInitErr() != NIXL_SUCCESS) {
            // Return null - calling test should skip
            return nullptr;
        }

        return engine;
    }

    nixlBackendInitParams init_params;
    std::map<std::string, std::string> custom_params;
};

TEST_F(OfiBackendTest, ConstructorBasic) {
    auto engine = createTestEngine();
    if (!engine) {
        GTEST_SKIP() << "OFI backend initialization failed - test environment likely lacks proper libfabric setup";
    }

    EXPECT_NE(engine.get(), nullptr);
    EXPECT_EQ(engine->getInitErr(), NIXL_SUCCESS);
}

TEST_F(OfiBackendTest, SupportMethods) {
    auto engine = createTestEngine();
    if (!engine) {
        GTEST_SKIP() << "OFI backend initialization failed - skipping support methods test";
    }

    EXPECT_TRUE(engine->supportsNotif());
    EXPECT_TRUE(engine->supportsRemote());
    EXPECT_TRUE(engine->supportsLocal());
    EXPECT_TRUE(engine->supportsProgTh());
}

TEST_F(OfiBackendTest, GetSupportedMems) {
    auto engine = createTestEngine();
    if (!engine) {
        GTEST_SKIP() << "OFI backend initialization failed - skipping memory support test";
    }

    nixl_mem_list_t mems = engine->getSupportedMems();
    EXPECT_FALSE(mems.empty());
    EXPECT_EQ(mems[0], DRAM_SEG);
}

TEST_F(OfiBackendTest, InvalidProvider) {
    custom_params["provider"] = "invalid_provider";

    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OfiBackendTest, CustomTimeout) {
    custom_params["eq_timeout_ms"] = "500";

    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

// Helper functions for local operations testing
void allocateAndRegisterOfi(nixlOfiEngine* engine, int dev_id, nixl_mem_t mem_type,
                            void*& addr, size_t len, nixlBackendMD*& md) {
    nixlBlobDesc desc;

    // Allocate buffer - for simplicity, we'll use malloc for DRAM
    if (mem_type == DRAM_SEG) {
        addr = malloc(len);
        if (!addr) {
            throw std::runtime_error("Failed to allocate memory");
        }
    } else {
        throw std::runtime_error("Unsupported memory type for test");
    }

    desc.addr = reinterpret_cast<uintptr_t>(addr);
    desc.len = len;
    desc.devId = dev_id;

    nixl_status_t ret = engine->registerMem(desc, mem_type, md);
    if (ret != NIXL_SUCCESS) {
        free(addr);
        throw std::runtime_error("Failed to register memory");
    }
}

void deallocateAndDeregisterOfi(nixlOfiEngine* engine, int dev_id, nixl_mem_t mem_type,
                                void* addr, nixlBackendMD* md) {
    engine->deregisterMem(md);
    if (mem_type == DRAM_SEG) {
        free(addr);
    }
}

void populateDescsOfi(nixl_meta_dlist_t& descs, int dev_id, void* base_addr,
                      int desc_cnt, size_t desc_size, nixlBackendMD* md) {
    for (int i = 0; i < desc_cnt; i++) {
        nixlMetaDesc desc;
        desc.addr = reinterpret_cast<uintptr_t>(base_addr) + (i * desc_size);
        desc.len = desc_size;
        desc.devId = dev_id;
        desc.metadataP = md;
        descs.addDesc(desc);
    }
}

// Test intra-agent (local) transfer operations
void test_ofi_intra_agent_transfer(nixlOfiEngine* engine, nixl_mem_t mem_type) {
    std::cout << std::endl << std::endl;
    std::cout << "****************************************************" << std::endl;
    std::cout << "   OFI Intra-agent memory transfer test: " << std::endl;
    std::cout << "****************************************************" << std::endl;

    std::string agent_name("OfiLocalAgent");
    nixl_status_t ret;

    // Verify local support
    ASSERT_TRUE(engine->supportsLocal()) << "OFI backend should support local operations";

    // get connection info (needed even for local operations)
    std::string conn_info;
    ret = engine->getConnInfo(conn_info);
    ASSERT_EQ(ret, NIXL_SUCCESS) << "failed to get connection info";

    ret = engine->loadRemoteConnInfo(agent_name, conn_info);
    ASSERT_EQ(ret, NIXL_SUCCESS) << "failed to load local connection info";

    std::cout << "local connection setup complete" << std::endl;

    // Test parameters
    int desc_cnt = 4;
    size_t desc_size = 64 * 1024; // 64KB per descriptor
    size_t total_len = desc_cnt * desc_size;

    void* addr1;
    void* addr2;
    nixlBackendMD* lmd1;
    nixlBackendMD* lmd2;

    try {
        // Allocate and register memory regions
        allocateAndRegisterOfi(engine, 0, mem_type, addr1, total_len, lmd1);
        allocateAndRegisterOfi(engine, 0, mem_type, addr2, total_len, lmd2);

        // Test loadLocalMD - convert local metadata for use as remote
        nixlBackendMD* rmd2;
        ret = engine->loadLocalMD(lmd2, rmd2);
        ASSERT_EQ(ret, NIXL_SUCCESS) << "Failed to load local metadata";
        ASSERT_EQ(rmd2, lmd2) << "loadLocalMD should return same metadata for local operations";

        // Create transfer descriptors
        nixl_meta_dlist_t src_descs(mem_type);
        populateDescsOfi(src_descs, 0, addr1, desc_cnt, desc_size, lmd1);

        nixl_meta_dlist_t dst_descs(mem_type);
        populateDescsOfi(dst_descs, 0, addr2, desc_cnt, desc_size, rmd2);

        // Initialize source data with pattern
        memset(addr1, 0xAB, total_len);
        memset(addr2, 0x00, total_len);

        std::cout << "Testing local READ operation..." << std::endl;

        // Test READ operation (read from addr2 to addr1)
        // Note: This might not work without proper connection, but tests the interface
        nixlBackendReqH* read_handle;
        ret = engine->postXfer(NIXL_READ, src_descs, dst_descs, agent_name, read_handle);

        if (ret == NIXL_SUCCESS) {
            std::cout << "READ operation posted successfully" << std::endl;

            // Check transfer completion
            ret = engine->checkXfer(read_handle);
            // Don't assert on completion as it may depend on actual fabric setup

            engine->releaseReqH(read_handle);
        } else {
            std::cout << "READ operation posting failed (expected without full fabric setup): " << ret << std::endl;
        }

        std::cout << "Testing local WRITE operation..." << std::endl;

        // Test WRITE operation (write from addr1 to addr2)
        nixlBackendReqH* write_handle;
        ret = engine->postXfer(NIXL_WRITE, src_descs, dst_descs, agent_name, write_handle);

        if (ret == NIXL_SUCCESS) {
            std::cout << "WRITE operation posted successfully" << std::endl;

            // Check transfer completion
            ret = engine->checkXfer(write_handle);
            // Don't assert on completion as it may depend on actual fabric setup

            engine->releaseReqH(write_handle);
        } else {
            std::cout << "WRITE operation posting failed (expected without full fabric setup): " << ret << std::endl;
        }

        // Cleanup metadata
        engine->unloadMD(rmd2);

        // Cleanup memory
        deallocateAndDeregisterOfi(engine, 0, mem_type, addr1, lmd1);
        deallocateAndDeregisterOfi(engine, 0, mem_type, addr2, lmd2);

        std::cout << "Local operations test completed successfully" << std::endl;

    } catch (const std::exception& e) {
        // Cleanup on failure
        std::cout << "Test failed with exception: " << e.what() << std::endl;
        throw;
    }
}

TEST_F(OfiBackendTest, IntraAgentTransfer) {
    auto engine = createTestEngine();
    if (!engine) {
        GTEST_SKIP() << "OFI backend initialization failed - skipping local transfer test";
    }

    test_ofi_intra_agent_transfer(engine.get(), DRAM_SEG);
}

TEST_F(OfiBackendTest, LoadLocalMD) {
    auto engine = createTestEngine();
    if (!engine) {
        GTEST_SKIP() << "OFI backend initialization failed - skipping loadLocalMD test";
    }

    // Test loadLocalMD with valid metadata
    size_t buffer_size = 1024;
    void* buffer = malloc(buffer_size);
    ASSERT_NE(buffer, nullptr);

    nixlBlobDesc mem_desc;
    mem_desc.addr = reinterpret_cast<uintptr_t>(buffer);
    mem_desc.len = buffer_size;
    mem_desc.devId = 0;

    nixlBackendMD* input_md = nullptr;
    nixl_status_t status = engine->registerMem(mem_desc, DRAM_SEG, input_md);

    if (status == NIXL_SUCCESS) {
        ASSERT_NE(input_md, nullptr);

        // Test loadLocalMD
        nixlBackendMD* output_md = nullptr;
        status = engine->loadLocalMD(input_md, output_md);

        EXPECT_EQ(status, NIXL_SUCCESS);
        EXPECT_EQ(output_md, input_md) << "loadLocalMD should return same metadata for local operations";

        // Cleanup
        engine->deregisterMem(input_md);
    }

    free(buffer);
}

TEST_F(OfiBackendTest, LoadLocalMDNullInput) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    ASSERT_NE(engine.get(), nullptr);

    nixlBackendMD* output_md = nullptr;
    nixl_status_t status = engine->loadLocalMD(nullptr, output_md);

    // Should handle null input gracefully
    EXPECT_NE(status, NIXL_SUCCESS);
}

TEST_F(OfiBackendTest, UnloadMD) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    ASSERT_NE(engine.get(), nullptr);

    if (engine->getInitErr() == NIXL_SUCCESS) {
        // Test unloadMD with valid metadata
        size_t buffer_size = 1024;
        void* buffer = malloc(buffer_size);
        ASSERT_NE(buffer, nullptr);

        nixlBlobDesc mem_desc;
        mem_desc.addr = reinterpret_cast<uintptr_t>(buffer);
        mem_desc.len = buffer_size;
        mem_desc.devId = 0;

        nixlBackendMD* input_md = nullptr;
        nixl_status_t status = engine->registerMem(mem_desc, DRAM_SEG, input_md);

        if (status == NIXL_SUCCESS) {
            ASSERT_NE(input_md, nullptr);

            // Get a local metadata reference
            nixlBackendMD* local_md = nullptr;
            status = engine->loadLocalMD(input_md, local_md);
            ASSERT_EQ(status, NIXL_SUCCESS);

            // Test unloadMD
            status = engine->unloadMD(local_md);
            EXPECT_EQ(status, NIXL_SUCCESS) << "unloadMD should succeed for valid metadata";

            // Cleanup
            engine->deregisterMem(input_md);
        }

        free(buffer);
    } else {
        GTEST_SKIP() << "OFI backend initialization failed, skipping unloadMD test";
    }
}

TEST_F(OfiBackendTest, UnloadMDNullInput) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    ASSERT_NE(engine.get(), nullptr);

    // Test unloadMD with null input
    nixl_status_t status = engine->unloadMD(nullptr);
    EXPECT_EQ(status, NIXL_SUCCESS) << "unloadMD should handle null input gracefully";
}

TEST_F(OfiBackendTest, PostXferNullHandles) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    ASSERT_NE(engine.get(), nullptr);

    // Create empty descriptor lists for testing
    nixl_meta_dlist_t empty_src(DRAM_SEG);
    nixl_meta_dlist_t empty_dst(DRAM_SEG);

    // Test postXfer with null handle pointer
    nixlBackendReqH* handle = nullptr;
    nixl_status_t status = engine->postXfer(NIXL_READ, empty_src, empty_dst, "test_agent", handle);
    EXPECT_NE(status, NIXL_SUCCESS) << "postXfer should fail with empty descriptors";
}

TEST_F(OfiBackendTest, InvalidTimeout) {
    custom_params["eq_timeout_ms"] = "invalid";

    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OfiBackendTest, TimeoutOutOfRange) {
    custom_params["eq_timeout_ms"] = "70000";

    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OfiBackendTest, GetConnInfo) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string conn_info;
    nixl_status_t status = engine->getConnInfo(conn_info);

    EXPECT_EQ(status, NIXL_SUCCESS);
    EXPECT_FALSE(conn_info.empty());
}

TEST_F(OfiBackendTest, LoadRemoteConnInfo) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string remote_agent = "remote_test_agent";
    std::string conn_info = "dummy_connection_info";

    nixl_status_t status = engine->loadRemoteConnInfo(remote_agent, conn_info);
    EXPECT_EQ(status, NIXL_SUCCESS);
}

TEST_F(OfiBackendTest, ConnectWithoutRemoteInfo) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string remote_agent = "nonexistent_agent";
    nixl_status_t status = engine->connect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OfiBackendTest, DisconnectNonexistentAgent) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string remote_agent = "nonexistent_agent";
    nixl_status_t status = engine->disconnect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OfiBackendTest, RegisterMemoryDRAM) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    size_t buffer_size = 1024;
    void* buffer = malloc(buffer_size);
    ASSERT_NE(buffer, nullptr);

    nixlBlobDesc mem_desc;
    mem_desc.addr = reinterpret_cast<uintptr_t>(buffer);
    mem_desc.len = buffer_size;
    mem_desc.devId = 0;

    nixlBackendMD* metadata = nullptr;
    nixl_status_t status = engine->registerMem(mem_desc, DRAM_SEG, metadata);

    if (status == NIXL_SUCCESS) {
        EXPECT_NE(metadata, nullptr);

        nixl_status_t deregister_status = engine->deregisterMem(metadata);
        EXPECT_EQ(deregister_status, NIXL_SUCCESS);
    }

    free(buffer);
}

TEST_F(OfiBackendTest, DeregisterNullMetadata) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    nixl_status_t status = engine->deregisterMem(nullptr);
    EXPECT_EQ(status, NIXL_SUCCESS);
}

TEST_F(OfiBackendTest, CheckXferWithNullHandle) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    nixl_status_t status = engine->checkXfer(nullptr);
    EXPECT_EQ(status, NIXL_SUCCESS);
}

TEST_F(OfiBackendTest, ReleaseReqHWithNullHandle) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    nixl_status_t status = engine->releaseReqH(nullptr);
    EXPECT_EQ(status, NIXL_SUCCESS);
}

class OfiShmProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_params.localAgent = "shm_test_agent";
        custom_params["provider"] = "shm";
        custom_params["eq_timeout_ms"] = "100";
        init_params.customParams = reinterpret_cast<nixl_b_params_t*>(&custom_params);
    }

    void TearDown() override {
    }

    nixlBackendInitParams init_params;
    std::map<std::string, std::string> custom_params;
};

TEST_F(OfiShmProviderTest, SHMProviderDetection) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OfiShmProviderTest, SHMConnectionlessConnect) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string remote_agent = "shm_remote_agent";
    std::string conn_info = "dummy_shm_address";

    nixl_status_t status = engine->loadRemoteConnInfo(remote_agent, conn_info);
    EXPECT_EQ(status, NIXL_SUCCESS);

    status = engine->connect(remote_agent);
    EXPECT_TRUE(status == NIXL_SUCCESS || status == NIXL_ERR_BACKEND);
}

TEST_F(OfiShmProviderTest, SHMConnectionlessDisconnect) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    std::string remote_agent = "nonexistent_shm_agent";
    nixl_status_t status = engine->disconnect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OfiShmProviderTest, SHMSupportMethods) {
    std::unique_ptr<nixlOfiEngine> engine(new nixlOfiEngine(&init_params));

    EXPECT_TRUE(engine->supportsNotif());
    EXPECT_TRUE(engine->supportsRemote());
    EXPECT_TRUE(engine->supportsLocal());
    EXPECT_TRUE(engine->supportsProgTh());
}

class OfiMetadataTest : public ::testing::Test {
protected:
    void SetUp() override {
        metadata = new nixlOfiMetadata();
    }

    void TearDown() override {
        delete metadata;
    }

    nixlOfiMetadata* metadata;
};

TEST_F(OfiMetadataTest, DefaultConstructor) {
    EXPECT_EQ(metadata->mr, nullptr);
    EXPECT_EQ(metadata->desc, nullptr);
}

class OfiRequestTest : public ::testing::Test {
protected:
    void SetUp() override {
        request = new nixlOfiRequest(0);
    }

    void TearDown() override {
        delete request;
    }

    nixlOfiRequest* request;
};

TEST_F(OfiRequestTest, DefaultConstructor) {
    EXPECT_EQ(request->total_operations, 0);
    EXPECT_EQ(request->completed_operations, 0);
    EXPECT_TRUE(request->isComplete());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
