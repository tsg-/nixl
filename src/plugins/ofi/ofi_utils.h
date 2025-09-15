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
#ifndef NIXL_SRC_PLUGINS_OFI_OFI_UTILS_H
#define NIXL_SRC_PLUGINS_OFI_OFI_UTILS_H

#include "nixl_types.h"
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_cm.h>

// ofi utility functions and classes

class nixlOfiUtils {
public:
    // ofi initialization methods
    static nixl_status_t initOfi();
    static void cleanupOfi();

    // fabric discovery and setup
    static nixl_status_t setupFabric(const nixl_b_params_t& params);
    static nixl_status_t cleanupFabric();

    // fabric resources
    static struct fi_info *hints;
    static struct fi_info *fi;
    static struct fid_fabric *fabric;
    static struct fid_domain *domain;
    static struct fid_ep *ep;
    static struct fid_cq *txcq;
    static struct fid_cq *rxcq;
    static struct fid_av *av;
    static bool fabric_initialized;

private:
};

// placeholder for ofi status conversion
nixl_status_t ofi_status_to_nixl(int ofi_status);

// get common ofi backend options
nixl_b_params_t get_ofi_backend_common_options();

// helper function to get parameter values with defaults
int get_param_int(const nixl_b_params_t& params, const std::string& key, int default_value);

std::string get_param_string(const nixl_b_params_t& params, const std::string& key, const std::string& default_value);

// convert binary address data to human-readable string
std::string addr_to_string(const void* addr_data, size_t addr_len);

// Manual progress driving for FI_PROGRESS_MANUAL providers
void drive_manual_progress();

#endif