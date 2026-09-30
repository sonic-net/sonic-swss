/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ASAN process bootstrap. Adds code for ASAN builds, including:
 * - a constructor that wires our custom swss_asan_init_impl() into the process
 *   initialization that occurs before main().
 * - default LSan suppressions
 *
 * Include this file only in ASAN builds where you want that functionality
 * enabled (ENABLE_ASAN=y daemon targets). To run unit tests against the
 * implementation in asan.cpp in a non-ASAN build without installing a SIGTERM
 * handler, injecting a test leak, or pulling in sanitizer symbols, leave this
 * file out of the build and call swss_asan_init_impl() with test doubles for
 * the dependencies.
 */

#include "asan.h"

#include <unistd.h>

#include <cstdlib>
#include <sanitizer/lsan_interface.h>

// Configure default LSan suppressions.
extern "C" {
    const char* __lsan_default_suppressions() {
        return "leak:__static_initialization_and_destruction_0\n";
    }
}

// Wire swss_asan_init_impl() into the process initialization that occurs
// before main().
__attribute__((constructor))
static void swss_asan_init()
{
    if (!swss_asan_init_impl(::sigaction, ::access, std::malloc, __lsan_do_leak_check))
    {
        exit(EXIT_FAILURE);
    }
}
