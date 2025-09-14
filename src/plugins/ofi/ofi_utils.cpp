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

#include "ofi_utils.h"
#include "common/nixl_log.h"

nixl_status_t nixlOfiUtils::initOfi() {
    // placeholder ofi initialization
    NIXL_INFO << "initializing ofi utilities";
    return NIXL_SUCCESS;
}

void nixlOfiUtils::cleanupOfi() {
    // placeholder ofi cleanup
    NIXL_INFO << "cleaning up ofi utilities";
}

nixl_status_t nixlOfiUtils::setupFabric() {
    // placeholder fabric setup
    NIXL_INFO << "setting up ofi fabric";
    return NIXL_SUCCESS;
}

nixl_status_t nixlOfiUtils::cleanupFabric() {
    // placeholder fabric cleanup
    NIXL_INFO << "cleaning up ofi fabric";
    return NIXL_SUCCESS;
}

nixl_status_t ofi_status_to_nixl(int ofi_status) {
    // placeholder status conversion
    switch (ofi_status) {
    case 0: // fi_success equivalent
        return NIXL_SUCCESS;
    case -11: // -eagain equivalent
        return NIXL_IN_PROG;
    default:
        return NIXL_ERR_BACKEND;
    }
}

nixl_b_params_t get_ofi_backend_common_options() {
    // common ofi backend options
    nixl_b_params_t params = {
        {"ofi_provider", "verbs;ofi_rxm"},  // default to verbs with rxm utility provider
        {"ofi_domain", ""},                 // domain selection
        {"num_workers", "1"},               // number of worker threads
        {"tx_cq_size", "1024"},             // transmit completion queue size
        {"rx_cq_size", "1024"}              // receive completion queue size
    };
    return params;
}