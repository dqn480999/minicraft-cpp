#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define MC_MOD_EXPORT __declspec(dllexport)
#else
#define MC_MOD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MC_MOD_ABI_VERSION 1u

typedef enum McModValueType {
    MC_MOD_VALUE_INTEGER = 0,
    MC_MOD_VALUE_STRING = 1
} McModValueType;

typedef enum McModLogLevel {
    MC_MOD_LOG_INFO = 0,
    MC_MOD_LOG_WARN = 1,
    MC_MOD_LOG_ERROR = 2
} McModLogLevel;

typedef struct McModValue {
    int32_t type;
    int64_t integer;
    const char* string;
} McModValue;

typedef struct McModCommandContext {
    const char* sender;
    int32_t entity_id;
    int32_t is_op;
    const char* args;
} McModCommandContext;

typedef void (*McModEventCallback)(void* user, const char* event, const McModValue* args, size_t argc);
typedef void (*McModCommandCallback)(void* user, const McModCommandContext* context);

typedef struct McModApi {
    uint32_t abi_version;
    void* host;
    void (*log)(void* host, int32_t level, const char* message);
    void (*broadcast)(void* host, const char* message);
    int64_t (*player_count)(void* host);
    int32_t (*is_op)(void* host, const char* name);
    int64_t (*get_block)(void* host, int32_t x, int32_t y, int32_t z);
    void (*set_block)(void* host, int32_t x, int32_t y, int32_t z, uint16_t state);
    uint64_t (*register_event)(void* host, const char* event, McModEventCallback callback, void* user);
    void (*unregister_event)(void* host, uint64_t id);
    uint64_t (*register_command)(void* host, const char* name, const char* description, McModCommandCallback callback, void* user);
} McModApi;

typedef int (*McModInitFn)(const McModApi* api);
typedef void (*McModStartFn)(void);
typedef void (*McModStopFn)(void);
typedef void (*McModShutdownFn)(void);
typedef const char* (*McModNameFn)(void);
typedef const char* (*McModVersionFn)(void);

#ifdef __cplusplus
}
#endif
