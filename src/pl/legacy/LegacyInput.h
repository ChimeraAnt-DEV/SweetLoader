#pragma once

/**
 * @file LegacyInput.h
 * @brief The C input interface (`GetSweetLoaderInput`).
 *
 * <p><b>Still in use.</b> The sweetloader's own {@link GetInputInterface} builds a
 * `SweetLoaderInput_Interface`, and the C accessor is exported for mods built against it. The
 * callbacks it carries are the same ones {@link pl::runtime} dispatches to; see
 * `pl/runtime/InputBridge.h` for the C++ side.
 */

#include <stdbool.h>
#include <stddef.h>

#include "pl/legacy/LegacyMacro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*SweetLoaderInput_OnTouch_Fn)(int action, int pointerId, float x,
                                          float y);
typedef bool (*SweetLoaderInput_OnKeyEvent_Fn)(int keyCode,
                                             unsigned int unicodeChar,
                                             bool isKeyDown);
typedef bool (*SweetLoaderInput_OnTextInput_Fn)(const char *text, size_t length);
typedef bool (*SweetLoaderInput_OnMouse_Fn)(int button, bool isDown);

typedef struct SweetLoaderInput_Interface {
  void (*RegisterTouchCallback)(SweetLoaderInput_OnTouch_Fn callback);
  void (*RegisterKeyEventCallback)(SweetLoaderInput_OnKeyEvent_Fn callback);
  void (*ShowKeyboard)(void);
  void (*HideKeyboard)(void);
  void (*RegisterMouseCallback)(SweetLoaderInput_OnMouse_Fn callback);
  void (*RegisterTextInputCallback)(SweetLoaderInput_OnTextInput_Fn callback);
} SweetLoaderInput_Interface;

PL_LEGACY_EXPORT SweetLoaderInput_Interface *GetSweetLoaderInput(void);

#ifdef __cplusplus
} // extern "C"
#endif
