/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Testable ASAN helpers. Production daemons enable them via the constructor in
 * asan_ctor.cpp; unit tests call swss_asan_init_impl() /
 * swss_asan_sigterm_handler_impl() with injected dependencies and leave
 * asan_ctor.cpp out of the link.
 */

#pragma once

#include <cstddef>
#include <signal.h>

// Unique size to identify the intentional ASAN test-leak allocation in reports.
static constexpr size_t SWSS_ASAN_TEST_LEAK_SIZE = 9861842;

// Function pointers for passing real implementations or test doubles.
// sigaction() from signal.h
using SwssSigactionFn = int (*)(int, const struct sigaction *, struct sigaction *);
// access() from unistd.h
using SwssAccessFn = int (*)(const char *, int);
// malloc() from stdlib.h
using SwssMallocFn = void *(*)(size_t);
// __lsan_do_leak_check() from sanitizer/lsan_interface.h
using SwssLsanLeakCheckFn = void (*)(void);
// _exit() from unistd.h
using SwssExitFn = void (*)(int);
// raise() from signal.h
using SwssRaiseFn = int (*)(int);

// SIGTERM handler installed by swss_asan_init_impl(). Thin wrapper around
// swss_asan_sigterm_handler_impl() that passes g_lsan_leak_check and the real
// libc entry points. Exposed so tests can verify the handler pointer that was
// passed to sigaction.
void swss_asan_sigterm_handler(int signo);

// Testable SIGTERM-handler body. Production wrapper passes g_lsan_leak_check,
// ::sigaction, ::_exit, and ::raise; unit tests inject doubles.
void swss_asan_sigterm_handler_impl(int signo,
                                    SwssLsanLeakCheckFn leak_check_fn,
                                    SwssSigactionFn sigaction_fn,
                                    SwssExitFn exit_fn,
                                    SwssRaiseFn raise_fn);

// Allocate (and never free) the intentional test leak via malloc_fn.
void swss_asan_inject_test_leak(SwssMallocFn malloc_fn);

// Set up custom machinery for ASAN builds.
// - Installs a SIGTERM handler for checking for leaks.
// - When /etc/sonic/inject_asan_test_leak_enabled exists, injects a known
//   test leak via malloc_fn.
// - Returns false if signal-handler installation fails; true otherwise (including
//   when leak injection is skipped or malloc_fn returns nullptr).
bool swss_asan_init_impl(SwssSigactionFn sigaction_fn,
                         SwssAccessFn access_fn,
                         SwssMallocFn malloc_fn,
                         SwssLsanLeakCheckFn leak_check_fn);
