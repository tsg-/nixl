/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2025 Amazon.com, Inc. and affiliates.
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

#include "libfabric_common.h"
#include "common/nixl_log.h"

#include <iomanip>
#include <sstream>
#include <atomic>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>

namespace LibfabricUtils {

std::vector<std::string>
getAvailableEfaDevices() {
    return getAvailableLibfabricDevices("efa");
}

std::vector<std::string>
getAvailableLibfabricDevices(const char* provider_name) {
    std::vector<std::string> devices;
    struct fi_info *hints, *info;
    hints = fi_allocinfo();
    if (!hints) {
        NIXL_ERROR << "Failed to allocate fi_info for device discovery";
        return devices;
    }

    // Check for FI_PROVIDER environment variable first
    const char* env_provider = getenv("FI_PROVIDER");
    const char* actual_provider = provider_name;

    if (env_provider && strlen(env_provider) > 0) {
        actual_provider = env_provider;
        NIXL_DEBUG << "Using FI_PROVIDER environment variable: " << env_provider;
        if (provider_name && strcmp(provider_name, env_provider) != 0) {
            NIXL_DEBUG << "Note: FI_PROVIDER (" << env_provider << ") overrides requested provider (" << provider_name << ")";
        }
    }

    // Set provider name if specified, otherwise discover all providers
    if (actual_provider) {
        hints->fabric_attr->prov_name = strdup(actual_provider);
        NIXL_DEBUG << "Discovering devices for provider: " << actual_provider;
    } else {
        NIXL_DEBUG << "Discovering devices for all providers";
    }

    int ret = fi_getinfo(FI_VERSION(1, 9), NULL, NULL, 0, hints, &info);
    if (ret) {
        NIXL_DEBUG << "fi_getinfo failed during device discovery for provider "
                  << (actual_provider ? actual_provider : "all") << ": " << fi_strerror(-ret);
        fi_freeinfo(hints);
        return devices;
    }

    for (struct fi_info *cur = info; cur; cur = cur->next) {
        if (cur->domain_attr && cur->domain_attr->name) {
            std::string device_name = cur->domain_attr->name;
            std::string prov_name = cur->fabric_attr->prov_name ? cur->fabric_attr->prov_name : "unknown";

            // Add provider prefix to device name for non-EFA devices
            if (prov_name != "efa") {
                device_name = prov_name + ":" + device_name;
            }

            if (std::find(devices.begin(), devices.end(), device_name) == devices.end()) {
                devices.push_back(device_name);
                NIXL_DEBUG << "Found device: " << device_name << " (provider: " << prov_name << ")";
            }
        }
    }

    fi_freeinfo(info);
    fi_freeinfo(hints);

    NIXL_DEBUG << "Discovered " << devices.size() << " devices for provider "
               << (actual_provider ? actual_provider : "all");
    return devices;
}

std::string
hexdump(const void *data) {
    static constexpr uint HEXDUMP_MAX_LENGTH = 56;
    std::stringstream ss;
    ss.str().reserve(HEXDUMP_MAX_LENGTH * 3);
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < HEXDUMP_MAX_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]) << " ";
    }
    return ss.str();
}

// Simple counter for pre-allocation only
static uint32_t g_xfer_id_counter = 1; // Start from 1, 0 reserved for special cases

std::vector<uint32_t>
preallocateXferIds(size_t count) {
    std::vector<uint32_t> xfer_ids;
    xfer_ids.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        uint32_t xfer_id = g_xfer_id_counter++;

        // Handle wraparound: 20-bit field can hold 0 to 1,048,575
        if (xfer_id > NIXL_XFER_ID_MASK) {
            // Reset counter and try again
            g_xfer_id_counter = 1;
            xfer_id = 1;
            g_xfer_id_counter = 2; // Update for next iteration
        }

        xfer_ids.push_back(xfer_id);
    }

    return xfer_ids;
}

} // namespace LibfabricUtils
