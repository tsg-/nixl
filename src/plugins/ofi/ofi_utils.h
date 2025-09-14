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

// placeholder ofi utility functions and classes

class nixlOfiUtils {
public:
    // placeholder utility methods
    static nixl_status_t initOfi();
    static void cleanupOfi();

    // placeholder for fabric discovery and setup
    static nixl_status_t setupFabric();
    static nixl_status_t cleanupFabric();
};

// placeholder for ofi status conversion
nixl_status_t ofi_status_to_nixl(int ofi_status);

// get common ofi backend options
nixl_b_params_t get_ofi_backend_common_options();

#endif