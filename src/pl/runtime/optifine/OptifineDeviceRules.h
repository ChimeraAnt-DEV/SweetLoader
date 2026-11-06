#pragma once

#include <cstdint>

/**
 * @file OptifineDeviceRules.h
 * @brief Pure rules that decide what the host optimizations may do on a device.
 *
 * Kept free of Android and syscalls so the decisions are unit-testable on the host: a wrong
 * "pin to these cores" rule is otherwise only visible as a device that boots but stutters.
 */
namespace pl::runtime {

/** Highest core count this suite will ever consider pinning to. */
constexpr int kOptifineMaxCores = 64;

/** Minimum number of performance cores before CPU pinning is worth doing. */
constexpr int kOptifineMinPerformanceCores = 4;

/** A core counts as "performance" when its max frequency is at least this fraction of the
 *  highest core's. Kept for reference; {@link BigClusterCoreCount} uses the largest frequency gap
 *  instead, because a fixed ratio misses tri-cluster gold cores (see its note). */
constexpr double kOptifinePerformanceCoreRatio = 0.9;

/**
 * @brief The number of cores in the big (fastest) cluster.
 *
 * <p>The cluster is found by the largest gap in the sorted frequency table, not by a fixed ratio of
 * the fastest core. A tri-cluster SoC often has its gold cores below 0.9 of the prime core (a
 * Snapdragon 8 Gen 1's 2.5GHz gold vs 3.0GHz prime is 0.83), so a 0.9 rule counts only the single
 * prime core and the "need 4+" check then refuses to pin, silently doing nothing.
 *
 * @param maxFreqKhz per-core maximum frequency in kHz; 0 means unknown for that core.
 * @param count number of entries.
 * @return the number of cores in the big cluster, or 0 when there is no gap to cluster on (every
 *         core equal, or no known frequency) -- the caller must then skip pinning.
 */
int BigClusterCoreCount(const std::uint64_t *maxFreqKhz, int count);

/**
 * @brief Writes the indices of the big cluster's cores, in core order.
 *
 * @param maxFreqKhz per-core maximum frequency in kHz; 0 means unknown.
 * @param count number of entries.
 * @param out receives the selected core indices.
 * @param maxOut capacity of @p out.
 * @return the number of indices written, or 0 when there is no cluster to select.
 */
int SelectBigClusterCoreIndices(const std::uint64_t *maxFreqKhz, int count, int *out, int maxOut);

/**
 * @brief Counts the performance cores in a frequency table.
 *
 * @deprecated Superseded by {@link BigClusterCoreCount}, which handles tri-cluster gold cores. Kept
 * so the historical 0.9-ratio behaviour stays available and tested.
 */
int CountPerformanceCores(const std::uint64_t *maxFreqKhz, int count);

/**
 * @brief Whether the device has enough performance cores to make pinning worthwhile.
 */
bool ShouldPinToPerformanceCores(int performanceCoreCount);

/**
 * @brief Builds a CPU affinity mask from the indices of the cores to pin to.
 *
 * @param coreIndices core indices to include (values outside 0..63 are ignored).
 * @param count number of indices.
 * @return a bitmask with one bit per selected core, or 0 when nothing valid was selected.
 */
std::uint64_t BuildCoreMask(const int *coreIndices, int count);

/**
 * @brief The refresh rate the suite should request.
 *
 * @param supportedHz the display's supported refresh rates, ascending; may be empty.
 * @param count number of entries.
 * @param currentHz the rate the game is currently using, or 0 if unknown.
 * @return the highest supported rate above {@code currentHz}, or 0 when there is nothing to
 *         unlock (a 60Hz-only panel, an unknown list, or the game already at the top rate).
 */
int SelectTargetRefreshRate(const int *supportedHz, int count, int currentHz);

} // namespace pl::runtime
