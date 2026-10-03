/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_KTEST_H
#define MAKHOS_KTEST_H

#include <types.h>

/**
 * =============================================================================
 * ktest.h - In-kernel test framework
 * =============================================================================
 * Tests register themselves at link time via a dedicated section, so adding a
 * test is just writing a KTEST(name) { ... } block anywhere in the tree - no
 * central list to update. The runner executes every registered test, prints a
 * PASS/FAIL line for each, and returns the number of failures.
 *
 * With the "makh.test" boot argument, kernel_main runs the suite and powers the
 * machine off with an exit code (0 = all passed) via QEMU's isa-debug-exit
 * device, which is how `make test` and CI decide success.
 *
 * Assertions (KEXPECT*) record a failure and keep going, so one test can report
 * many problems in a single run. A test "passes" iff it records zero failures.
 * =============================================================================
 */

typedef void (*ktest_fn_t)(void);

typedef struct ktest {
    const char* name;
    const char* suite;
    ktest_fn_t  fn;
} ktest_t;

/* The .ktests section is a table of pointers to descriptors (see KTEST). */
typedef const ktest_t* ktest_ptr_t;

/*
 * Register a test. We place a POINTER to the descriptor in the .ktests section
 * (the Linux initcall pattern) rather than the descriptor itself: GCC may
 * over-align a >=16-byte struct to 16 bytes, which would leave gaps between
 * array elements and break the runner's stride. Pointers are always 8 bytes,
 * so the section is a clean, tightly packed table.
 */
#define KTEST(suite_name, test_name)                                          \
    static void ktest_##suite_name##_##test_name(void);                       \
    static const ktest_t __ktest_desc_##suite_name##_##test_name = {          \
        #test_name, #suite_name, ktest_##suite_name##_##test_name };          \
    static const ktest_t* const __ktest_ptr_##suite_name##_##test_name        \
        __attribute__((used, section(".ktests"))) =                           \
        &__ktest_desc_##suite_name##_##test_name;                             \
    static void ktest_##suite_name##_##test_name(void)

/* Assertion primitives - implemented in ktest.c. */
void ktest_record_fail(const char* file, int line, const char* expr);
void ktest_record_check(void);

/* Number of failures recorded by the currently running test. */
int  ktest_current_failures(void);

#define KEXPECT(cond) do {                                                    \
        ktest_record_check();                                                 \
        if (!(cond)) ktest_record_fail(__FILE__, __LINE__, #cond);            \
    } while (0)

#define KEXPECT_EQ(a, b) do {                                                 \
        ktest_record_check();                                                 \
        if ((int64_t)(a) != (int64_t)(b))                                     \
            ktest_record_fail(__FILE__, __LINE__, #a " == " #b);              \
    } while (0)

#define KEXPECT_NE(a, b) do {                                                 \
        ktest_record_check();                                                 \
        if ((int64_t)(a) == (int64_t)(b))                                     \
            ktest_record_fail(__FILE__, __LINE__, #a " != " #b);              \
    } while (0)

/* Abort the current test immediately (e.g. a NULL that would crash later). */
#define KASSERT_TEST(cond) do {                                               \
        ktest_record_check();                                                 \
        if (!(cond)) { ktest_record_fail(__FILE__, __LINE__, #cond); return; }\
    } while (0)

/* Run every registered test. Returns the number of failed tests. */
int ktest_run_all(void);

/* Run only tests whose suite name matches. Returns failed test count. */
int ktest_run_suite(const char* suite);

#endif /* MAKHOS_KTEST_H */
