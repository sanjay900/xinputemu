#pragma once
#include <wbemidl.h>
#include <oleauto.h>
#ifdef __cplusplus
extern "C" {
#endif
BOOL IsXInputDevice(const GUID *pGuidProductFromDirectInput);
#ifdef __cplusplus
}
#endif