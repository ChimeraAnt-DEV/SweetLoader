#include "pl/runtime/LogRing.hpp"

namespace pl::runtime {

LogRing &SharedLogRing() {
  // Function-local static: constructed on first use and destroyed at exit, in the order the language
  // guarantees, so a logger used from a static destructor still finds a live ring.
  static LogRing ring;
  return ring;
}

} // namespace pl::runtime
