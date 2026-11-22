# Minicraft C++ Mod Loader

The server can load native C++ mods from shared libraries at startup. A mod is a `.so` file that exports a small C ABI and can optionally use the C++ wrapper in `src/mod/Mod.hpp`.

The mod loader is intentionally small:

- loads `.so` and `.dylib` files from `mod-dir`;
- calls `mc_mod_init` on each mod;
- lets mods register event callbacks and commands;
- exposes a bounded host API for logging, chat, player count, permissions and block access;
- calls optional start, stop and shutdown hooks;
- unloads libraries during shutdown.

Mods run inside the server process. They are trusted code.

## 1. Directory layout

```text
src/mod/ModApi.h       C ABI
src/mod/Mod.hpp        C++ wrapper
src/mod/ModManager.cpp mod loader and host API implementation
mods/hello_mod.cpp     example mod source
build/mods/*.so        built mod libraries
```

The server scans `mod-dir` for shared libraries. The default is:

```properties
mods-enabled=true
mod-dir=mods
```

Files are sorted by path before loading. Subdirectories are not scanned.

## 2. Build the example mod

The example mod is built by default:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The output is:

```text
build/mods/hello_mod.so
```

If the server is started from the repository root, copy the library into the
configured mod directory:

```bash
cp build/mods/hello_mod.so mods/
./build/minicraft --config config/server.properties --port 25566
```

Alternatively, run the server from the build directory:

```bash
cd build
./minicraft --config ../config/server.properties --port 25566
```

The second form finds `mods/hello_mod.so` because the working directory is the
build directory.

To skip the example:

```bash
cmake -S . -B build -DMINICRAFT_BUILD_EXAMPLE_MOD=OFF
```

## 3. Minimal mod

```cpp
#include "mod/Mod.hpp"

class MyMod final : public mc::mod::Mod {
protected:
    int onInit() override {
        on("on_join", [this](const mc::mod::Event& event) {
            broadcast("&eWelcome " + event.asString(0));
        });
        return 0;
    }
};

MC_MOD(MyMod)
```

Build it as a shared module with:

```cmake
add_library(my_mod MODULE my_mod.cpp)
target_include_directories(my_mod PRIVATE /path/to/minicraft/src)
set_target_properties(my_mod PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods")
```

`MC_MOD(MyMod)` exports:

```cpp
extern "C" int mc_mod_init(const McModApi* api);
extern "C" void mc_mod_start();
extern "C" void mc_mod_stop();
extern "C" void mc_mod_shutdown();
```

## 4. Lifecycle

### 4.1 Load

For each library:

1. `dlopen` with `RTLD_NOW | RTLD_LOCAL`.
2. Resolve `mc_mod_init`.
3. Resolve optional `mc_mod_name` and `mc_mod_version`.
4. Check the ABI version in the wrapper.
5. Call `mc_mod_init`.
6. Store the handle and optional start, stop and shutdown hooks.

If initialization returns non-zero, the library is unloaded and its registered
callbacks are removed.

### 4.2 Start

After all mods load, the server calls every `mc_mod_start` hook. The `on_server_start`
event is then fired to mods, followed by the OBLVN `on_server_start` event.

### 4.3 Runtime

Events are fired by the server as they happen. Commands registered by mods are
checked when a command is not recognized by the built-in command manager.

### 4.4 Stop

During shutdown:

1. The tick thread stops.
2. `on_server_stop` is fired to mods and scripts.
3. The network layer stops.
4. `mc_mod_stop` is called for every mod.
5. `mc_mod_shutdown` is called for every mod.
6. All event and command bindings are removed.
7. Libraries are unloaded with `dlclose`.

### 4.5 No hot reload

Mods are not hot reloaded. To load a new build, stop the server, replace the
library and start the server again.

## 5. C++ wrapper

Include:

```cpp
#include "mod/Mod.hpp"
```

### 5.1 Mod class

Override any of:

```cpp
int onInit();
void onStart();
void onStop();
void onShutdown();
```

`onInit` runs inside `mc_mod_init`. Register event callbacks and commands there.
Return non-zero to abort loading.

### 5.2 Registration

```cpp
std::uint64_t id = on("on_join", [](const mc::mod::Event& event) {
    // ...
});

bool ok = command("hello", "Say hello", [](const mc::mod::CommandContext& context) {
    // ...
});
```

`on` returns an event binding id. `command` returns false when the command name
is already registered or the API is unavailable.

### 5.3 Event object

```cpp
const std::string& name = event.name();
std::size_t count = event.size();
bool isString = event.isString(index);
std::int64_t value = event.asInt(index);
const std::string& text = event.asString(index);
```

`asString` returns an empty string when the value is not a string or the index is
out of range. `asInt` returns zero in the same cases.

### 5.4 Command context

```cpp
struct CommandContext {
    std::string sender;
    std::int32_t entityId;
    bool isOp;
    std::string args;
};
```

The console sender is `console` and has `isOp = true`.

### 5.5 Host helpers

```cpp
info("message");
warn("message");
error("message");
broadcast("&echat message");
int64_t players = playerCount();
bool op = isOp("name");
int64_t state = getBlock(x, y, z);
setBlock(x, y, z, state);
```

`broadcast` sends a system chat message to all online players.

## 6. Event reference

Event values arrive in the same order as the OBLVN event arguments.

| Event | Values |
| --- | --- |
| `on_join` | string name, integer entity id |
| `on_leave` | string name |
| `on_chat` | string name, string message, integer entity id |
| `on_block_break` | integer x, y, z, block state |
| `on_block_place` | integer x, y, z, block state |
| `on_death` | string name, integer entity id |
| `on_command` | string command, string arguments, integer entity id |
| `on_tick` | integer script tick counter |
| `on_server_start` | none |
| `on_server_stop` | none |
| `on_player_damage` | integer entity id, integer damage, integer new health |
| `on_respawn` | integer entity id |
| `on_item_pickup` | integer player entity, item id, count, damage |
| `on_item_drop` | integer player entity, item id, count, damage |

Event names are case-insensitive; the loader normalizes them to lowercase.

## 7. Example mod

`mods/hello_mod.cpp` demonstrates:

- `on_join` welcome with the player name;
- `on_server_start` and `on_server_stop` logging;
- `/hello` public command;
- `/modblock` operator command that places a stone block.

After loading, `/mods` lists loaded mods.

## 8. Command behavior

Mod commands are checked after built-in commands. A mod cannot overwrite a
built-in command name. Duplicate mod command names are rejected.

Command names are normalized to lowercase. The command manager passes the text
without the leading slash as `args`.

Example:

```text
/hello world
```

produces:

```cpp
context.sender   // player name or "console"
context.entityId // player entity id, or 0 for console
context.isOp     // permission flag
context.args     // "world"
```

## 9. Host API

The C ABI is defined in `src/mod/ModApi.h`.

```c
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
    uint64_t (*register_command)(void* host, const char* name, const char* description,
                                 McModCommandCallback callback, void* user);
} McModApi;
```

### 9.1 Values

```c
typedef struct McModValue {
    int32_t type;
    int64_t integer;
    const char* string;
} McModValue;
```

`type` is `MC_MOD_VALUE_INTEGER` or `MC_MOD_VALUE_STRING`. String pointers are
valid only for the duration of the callback. Copy them if they must outlive the
callback.

### 9.2 Event callback

```c
typedef void (*McModEventCallback)(void* user, const char* event,
                                   const McModValue* args, size_t argc);
```

The callback must not throw across the C ABI. The wrapper catches C++ exceptions
in the loader boundary; native mods using the raw C ABI should catch their own.

### 9.3 Command callback

```c
typedef void (*McModCommandCallback)(void* user, const McModCommandContext* context);
```

The context and its strings are valid only for the duration of the callback.

### 9.4 ABI version

The current ABI version is `1`.

The wrapper refuses to initialize when `abi_version` does not match. Rebuild mods
when the ABI changes.

## 10. Threading

Event callbacks run on the thread that fired the event:

- `on_tick` runs on the server tick thread;
- join, leave, chat and block events run on network I/O threads;
- `on_server_start` and `on_server_stop` run on the startup/shutdown thread;
- mod commands run on the network I/O thread that received the command.

The loader serializes mod registration and binding lookup with a mutex, but it
does not serialize your mod's own state.

Rules for mod authors:

- avoid shared mutable state unless it is protected;
- do not block for a long time in an event callback;
- do not call back into the server from another thread without synchronization;
- do not assume all callbacks run on one thread;
- keep `on_tick` fast.

## 11. Security

Mods are native code. They can do anything the server process can do: read files,
open sockets, spawn processes and call any library. Only load mods you trust.

The host API is not a sandbox. It is a convenience interface.

Recommendations:

- keep the mod directory writable only by the administrator;
- review mod source before deploying;
- build mods from trusted source;
- run the server as an unprivileged user;
- use filesystem permissions and process isolation if running untrusted code.

## 12. Limitations

- No hot reload.
- No event cancellation.
- No per-player targeting in the host API.
- No scheduler or delayed tasks.
- No persistent storage API.
- No JSON or configuration API.
- No network API.
- No stable C++ ABI across compilers; use the C ABI for cross-compiler mods.
- Mods share the server process and its crash domain.

## 13. Comparison with OBLVN

| Feature | OBLVN | C++ mods |
| --- | --- | --- |
| Language | OBLVN bytecode | C++ |
| Hot reload | yes, `/script reload` | no |
| Execution limit | yes | no |
| Memory safety | VM bounds | native |
| Full server API | host calls only | host calls, plus any linked C++ |
| Build step | no | yes |
| Performance | VM | native |
| Trust level | configuration | native code |

Use OBLVN for small, reloadable event logic. Use C++ mods for performance-critical
or deeply integrated behavior.

## 14. Troubleshooting

### Mod not loaded

Check:

- `mods-enabled=true`;
- `mod-dir` points to the directory containing the `.so`;
- the library exports `mc_mod_init`;
- the server log for `dlopen` errors;
- the ABI version matches.

### Command not found

Check:

- the mod registered the command in `onInit`;
- the command name is not already a built-in;
- the command name is unique among mods;
- `/mods` lists the mod.

### Callback strings look corrupted

C ABI strings are only valid during the callback. Copy them into `std::string`.

### Crash after shutdown

A mod may be using callbacks after unload. Ensure event and command bindings are
removed during shutdown and do not keep pointers into the mod library after
`mc_mod_shutdown`.

### Mod works from build directory but not repository root

The mod directory is relative to the process working directory. Copy the `.so`
into the configured directory or run from the build directory.

## 15. Future extensions

Possible future additions:

- per-player message and health APIs;
- scheduled tasks;
- event cancellation;
- configuration files for mods;
- stable C++ ABI helper library;
- mod metadata manifest;
- dependency ordering.
