#ifndef MAKHOS_TESTS_H
#define MAKHOS_TESTS_H

/**
 * =============================================================================
 * tests/comprehensive_tests.h - Comprehensive Test Suite Header
 * =============================================================================
 */

/**
 * run_comprehensive_tests - Execute all comprehensive test scenarios
 * 
 * Runs 10 deep test scenarios:
 * 1. Process table stress test
 * 2. Fork bomb with memory verification
 * 3. Process tree relationships
 * 4. Scheduler state validation
 * 5. Idle process guarantee
 * 6. Multiple forks
 * 7. Process find by PID
 * 8. Process exit
 * 9. Parent-child relationships
 * 10. Full integration
 */
void run_comprehensive_tests(void);

#endif // MAKHOS_TESTS_H
