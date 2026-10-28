#pragma once
// Compatibility shim: main.cpp calls DobbyHook; this routes it to And64InlineHook.
#include "And64InlineHook.hpp"
typedef void (*dobby_dummy_func_t)(void);
static inline int DobbyHook(void* addr, dobby_dummy_func_t replace, dobby_dummy_func_t* origin) {
    A64HookFunction(addr, (void*)replace, (void**)origin);
    return 0;
}
