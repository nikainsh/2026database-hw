/* database26 - test-only crash injection hook */
#pragma once

/**
 * @brief Crash the process when MINIDB_CRASH_POINT matches `point`.
 *
 * This is a test hook for exercising crash-recovery paths. It is a no-op in
 * normal runs because the environment variable is absent. The process exits
 * without running destructors, simulating a power loss at that exact point.
 */
void fault_inject_point(const char *point);
