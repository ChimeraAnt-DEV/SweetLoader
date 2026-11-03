/**
 * @file FeatureAvailability.cpp
 * @brief The process-wide per-feature availability table.
 */

#include "pl/runtime/FeatureAvailability.hpp"

namespace pl::runtime {

FeatureAvailability &GameFeatureAvailability() {
  // One table per process. Deliberately not `inline static` in the header: a function-local static
  // here has a single definition, and the install path and the JNI reader both reach the same
  // instance through this accessor.
  static FeatureAvailability table;
  return table;
}

} // namespace pl::runtime
