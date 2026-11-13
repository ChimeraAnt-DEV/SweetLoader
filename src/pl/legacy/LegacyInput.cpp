#include "pl/legacy/LegacyInput.h"

#include "pl/runtime/InputBridge.h"

extern "C" {

PL_LEGACY_EXPORT SweetLoaderInput_Interface *GetSweetLoaderInput() {
  return pl::runtime::GetInputInterface();
}

} // extern "C"

