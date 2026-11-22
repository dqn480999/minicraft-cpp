# Minicraft C++ Server

Experimental Minecraft Java 1.8.9 server core (protocol 47) written in C++20.

The project implements the network protocol, world storage, inventory, survival mechanics, mobs and a small generated village without a Minecraft server dependency.

## Features

- epoll networking with multiple I/O threads, `SO_REUSEPORT` and `TCP_DEFER_ACCEPT`
- named I/O threads and optional CPU affinity (`pin-io-threads`)
- optional NASM fast paths for VarInt coding and memory copy/zero
- handshake, status, legacy ping, login (offline and Mojang online mode)
- zlib packet compression and AES-128/CFB8 encryption
- flat world with sparse chunk sections, saved modified chunks and player data
- bounded chunk cache, entity limits and per-connection output limits
- chunk streaming, block placement and breaking
- inventory, cursor, held item, item drops and pickup
- survival: health, hunger, regeneration, fall/lava/drowning/starvation damage, death and respawn
- falling sand and gravel
- water and lava spread and recede after source removal
- daylight and block light calculation
- passive mobs, villagers and a generated village
- chat colors and permission commands
- embedded OBLVN register bytecode for server events
- native C++ mod loader with a versioned C ABI
- autosave and server log file

## Requirements

- Linux (epoll)
- CMake 3.20 or newer
- Ninja or Make
- C++20 compiler (GCC 11+ or Clang 14+)
- zlib
- OpenSSL
- libcurl (optional, required for `online-mode=true`)
- pthreads
- NASM (optional, x86_64 fast paths)

Ubuntu / Debian:

```bash
sudo apt install build-essential cmake ninja-build zlib1g-dev libssl-dev libcurl4-openssl-dev nasm
```

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/minicraft --config config/server.properties --port 25566
```

The default port in `config/server.properties` is `25565`. Use `--port` if that port is already occupied.

Runtime files are created in the working directory:

- `server.log` — server log
- `world/` — saved chunks
- `playerdata/` — saved player state
- `ops.txt` — operators added at runtime

## Test

The probe client connects to a running server and checks status, login, chunks, inventory actions, drops, pickup, sand, fluid spread and receding, damage, respawn, mobs and eating.

```bash
./build/probe 127.0.0.1 25566 TestBot
```

Unit tests cover protocol primitives, chunks, lighting, persistence, assembly paths and the OBLVN compiler and virtual machine:

```bash
./build/core_test
```

The test profile uses faster hunger/time settings:

```bash
./build/minicraft --config config/test.properties --port 25566
./build/probe 127.0.0.1 25566 TestBot
```

## Scripting

OBLVN is a low-level register language compiled to bytecode and executed by the server core. Scripts in `scripts/*.obl` can handle `on_join`, `on_leave`, `on_chat`, `on_block_break`, `on_block_place`, `on_death`, `on_command`, `on_tick`, `on_server_start`, `on_server_stop`, `on_player_damage`, `on_respawn`, `on_item_pickup` and `on_item_drop`. They have 16 registers, persistent 4096-cell memory, byte and word memory access, indirect addressing, separate data and call stacks, carry and overflow flags, rotates, popcount, leading/trailing zero counts, byte swap, conditional gates, native calls for chat, player counts, time, block access and labeled integer broadcasts, and a configurable instruction limit. Operators can hot reload scripts with `/script reload` or dispatch an oracle with `/script run <oracle>`.

```obl
ARCANA "welcome" {
    ORACLE @on_join ~> {
        BIND($a0) ::= SIGIL("&eWelcome to the obelisk");
        SUMMON(#2);
        HALT;
    }
}
```

The syntax reference, instruction set, host calls, event arguments and limits are in [LANGUAGE.md](LANGUAGE.md). The bundled scripts include `welcome.obl`, `stats.obl`, `bitpack.obl`, `bitwise.obl`, `memory_map.obl` and the compile-checked `syntax_test.obl`. Disable scripting with `scripts-enabled=false`.

## C++ mods

The server loads native C++ mods from `mods/*.so` through a versioned C ABI. Mods can register event callbacks, add commands, broadcast chat, inspect players and permissions, and read or modify blocks. The example `mods/hello_mod.cpp` is built as `build/mods/hello_mod.so`.

```cpp
#include "mod/Mod.hpp"

class MyMod final : public mc::mod::Mod {
protected:
    int onInit() override {
        on("on_join", [this](const mc::mod::Event& event) {
            broadcast("&eWelcome " + event.asString(0));
        });
        command("hello", "Say hello", [this](const mc::mod::CommandContext& context) {
            broadcast("&aHello from " + context.sender);
        });
        return 0;
    }
};

MC_MOD(MyMod)
```

Build the example and copy it into the mod directory:

```bash
cmake --build build -j
cp build/mods/hello_mod.so mods/
```

The full API, lifecycle, event table, command behavior, threading rules, security notes and troubleshooting are in [MODDING.md](MODDING.md). Disable mods with `mods-enabled=false`.

## Configuration

`config/server.properties` contains the main settings. Important options:

- `port`, `host`, `online-mode`
- `max-players`, `view-distance`
- `gamemode`, `difficulty`, `motd`
- `compression-threshold`
- `io-threads`, `pin-io-threads`
- `ops`, `ops-file`, `name-replace`
- `world-save`, `world-dir`, `max-cached-chunks`
- `player-data`, `player-data-dir`
- `item-drops`, `item-pickup-delay-ms`, `item-pickup-radius`
- `sand-physics`, `fluids-enabled`, `fluid-tick-interval`, `fluid-updates-per-tick`
- `max-item-entities`, `max-falling-blocks`, `max-mobs`
- `max-packet-size`, `max-output-buffer`
- `hunger-interval-seconds`, `time-scale`
- `autosave-seconds`
- `scripts-enabled`, `script-dir`, `script-tick-interval-ms`
- `script-max-instructions`, `script-trace`, `script-disassemble`
- `mods-enabled`, `mod-dir`
- `log-file`

## Commands

Player and console commands:

```text
/help /list /info /say /me /gamemode /tp /time /seed /kick /spawn /stop
/op /deop /save /cleanup /give /clear /setblock /heal /feed /kill /summon /setfood
/script reload /script run <oracle>
/mods
```

Administrative commands require operator status. The console always has operator access.

## Project layout

```text
src/common      logging, config, JSON, crypto, UUID, thread pool
src/net         buffers, compression, encryption, connections, epoll loops
src/protocol    chat, authentication, item stacks
src/world       blocks, chunks, world, persistence
src/lang        OBLVN lexer, compiler, virtual machine, event engine
src/mod         C ABI, C++ wrapper, native mod loader
src/server      server, players, commands, mobs, entities
scripts         OBLVN event scripts
mods            C++ mod sources and built modules
tests           probe client and unit tests
config          server and test profiles
```

## Limitations

- Minecraft Java 1.8.9 only
- no full player physics or movement anti-cheat
- simple mob behavior without pathfinding or hostile spawning
- one generated village at a fixed location
- no crafting, armor effects or tool durability
- item NBT is not persisted
- light does not propagate across chunk boundaries
- no whitelist, ban list or TPS metrics
