# OBLVN

OBLVN is the low-level scripting language embedded in the Minicraft C++ server. It compiles to a compact register bytecode and runs inside the server tick and connection threads under a global script lock. Scripts live in `scripts/*.obl`.

The syntax is deliberately ornate. Every statement is explicit, every register is named, and every control transfer is written out. There is no expression precedence and no syntactic sugar beyond string constants.

## File shape

```obl
ARCANA "module-name" {
    ORACLE @oracle_name ~> {
        <statements>
    }
}
```

`ARCANA` groups one file. `ORACLE` declares a function. The oracle name `on_join`, `on_chat`, `on_tick`, `on_block_break`, and `on_leave` is called by the server when the matching event happens. Other oracle names can be called from OBLVN with `CALL`.

## Lexical rules

* Keywords are uppercase: `ARCANA`, `ORACLE`, `BIND`, `REVEAL`, `SCRIBE`, `VERIFY`, `GATE`, `WHEN`, `CALL`, `RETURN`, `PUSH`, `POP`, `SUMMON`, `HALT`, `NOP`, `SIGIL`, `MEM`, `GATE`.
* Registers are written `$r0` through `$r11` and `$a0` through `$a3`.
* Labels are written `@name`. A definition is `@name::`. A reference is `@name`.
* Integers are written `#42`, `#-7`, `#0`.
* Strings are written `"text"` with `\\`, `\"`, `\n`, `\t`, and `\r` escapes.
* Comments do not exist in OBLVN. Source is only code.
* Statements end with `;`.

## Registers and memory

`$r0` through `$r11` are general 64-bit signed registers. `$a0` through `$a3` are the argument registers and map to registers 12 through 15. Host calls read `$a0` through `$a3` and write their result to `$r0`.

Memory is a flat array of 4096 signed 64-bit cells. A memory access is always a base register plus a constant offset:

```obl
SCRIBE MEM[$r5 + #0] ::= $r2;
REVEAL($r3) ::= MEM[$r5 + #8];
```

The stack has 256 entries and is used by `PUSH`, `POP`, `CALL`, and `RETURN`. Every oracle starts with zeroed registers, zeroed memory, empty stack, cleared flags, and zeroed host arguments.

## Statements

### Bind

```obl
BIND($r0) ::= #10;
BIND($r1) ::= $r0;
BIND($r2) ::= SIGIL("literal");
BIND($r0) ::= $r1 <+> $r2;
BIND($r0) ::= $r0 <+> #1;
```

Arithmetic and bitwise operators:

| Operator | Meaning |
| --- | --- |
| `<+>` | add |
| `<->` | subtract |
| `<*>` | multiply |
| `</>` | signed divide |
| `<%>` | signed remainder |
| `<&>` | bitwise and |
| `<\|>` | bitwise or |
| `<^>` | bitwise xor |
| `<<>` | shift left |
| `<>>` | shift right |

The left side is a register. The right side may be a register, an integer, or `SIGIL("...")`. `SIGIL` returns a compile-time string id.

### Reveal and scribe

```obl
REVEAL($r2) ::= MEM[$r1 + #16];
SCRIBE MEM[$r1 + #16] ::= $r2;
SCRIBE MEM[$r1 + #16] ::= #-1;
```

Out-of-range memory traps the oracle at runtime.

### Verify

```obl
VERIFY($r0) <=> #50;
VERIFY($a0) <=> $r1;
```

`VERIFY` subtracts the right operand from the left operand conceptually and sets comparison flags to less, equal, or greater. It does not write a register.

### Conditional gate

```obl
WHEN::EQUAL => GATE @label;
WHEN::NOT_EQUAL => GATE @label;
WHEN::LESS => GATE @label;
WHEN::GREATER => GATE @label;
WHEN::LESS_OR_EQUAL => GATE @label;
WHEN::GREATER_OR_EQUAL => GATE @label;
WHEN::ZERO => GATE @label;
WHEN::NONZERO => GATE @label;
```

`LESS_EQUAL` and `GREATER_EQUAL` are accepted as aliases for `LESS_OR_EQUAL` and `GREATER_OR_EQUAL`.

### Unconditional gate and labels

```obl
GATE @done;
@done::
```

### Calls

```obl
CALL @helper;
RETURN;
```

`CALL` accepts a local label or an oracle name in the same file. `RETURN` exits the current call. `HALT` exits the whole oracle.

### Stack

```obl
PUSH($r3);
POP($r3);
```

### Host calls

```obl
BIND($a0) ::= #3;
SUMMON(#3);
```

`SUMMON` reads `$a0` through `$a3`. The result, when the host call has one, is written to `$r0`.

### Halt and nop

```obl
HALT;
NOP;
```

## Host calls

| Id | Name | Arguments | Result |
| --- | --- | --- | --- |
| 1 | broadcast integer | `$a0` value | none |
| 2 | broadcast string | `$a0` string id | none |
| 3 | player count | none | `$r0` |
| 4 | current time in milliseconds | none | `$r0` |
| 5 | get block | `$a0` x, `$a1` y, `$a2` z | `$r0` block state |
| 6 | set block | `$a0` x, `$a1` y, `$a2` z, `$a3` state | none |
| 7 | log integer | `$a0` value | none |

Block state is the 16-bit value used by the world: block id shifted left by four, plus metadata.

## Events

| Oracle | `$a0` | `$a1` | `$a2` | `$a3` |
| --- | --- | --- | --- | --- |
| `on_join` | player name string id | entity id | | |
| `on_leave` | player name string id | | | |
| `on_chat` | player name string id | message string id | entity id | |
| `on_block_break` | x | y | z | old block state |
| `on_block_place` | x | y | z | new block state |
| `on_death` | player name string id | entity id | | |
| `on_command` | command name string id | argument string id | entity id | |
| `on_tick` | script tick counter | | | |

Runtime strings use ids from 100000 upward. A compile-time `SIGIL` id belongs to the file's string table and is resolved by the same host call.

Every matching oracle in every loaded file runs. One event runs all scripts serially under the script lock.

## Limits and faults

* 4096 memory cells.
* 256 stack entries.
* 200000 instructions per oracle invocation.
* Division or remainder by zero traps.
* Out-of-range memory access traps.
* Unknown host id traps.
* Unknown oracle or label is a compile error.
* Traps are logged and the current oracle stops. The server keeps running.

## Complete example

```obl
ARCANA "welcome" {
    ORACLE @on_join ~> {
        BIND($a0) ::= SIGIL("&eWelcome to the obelisk");
        SUMMON(#2);
        HALT;
    }

    ORACLE @on_block_break ~> {
        BIND($r0) ::= $a3 </> #16;
        VERIFY($r0) <=> #50;
        WHEN::EQUAL => GATE @torch;
        GATE @done;
        @torch::
        BIND($a0) ::= SIGIL("&6A torch was extinguished");
        SUMMON(#2);
        @done::
        HALT;
    }
}
```

`welcome.obl` greets every joining player and announces a broken torch. `stats.obl` broadcasts the online player count once per minute:

```obl
ARCANA "stats" {
    ORACLE @on_tick ~> {
        BIND($r0) ::= $a0 <%> #60;
        VERIFY($r0) <=> #0;
        WHEN::NOT_EQUAL => GATE @quiet;
        SUMMON(#3);
        VERIFY($r0) <=> #0;
        WHEN::EQUAL => GATE @quiet;
        BIND($a0) ::= $r0;
        SUMMON(#1);
        @quiet::
        HALT;
    }
}
```

## Syntax showcase

`scripts/syntax_test.obl` is a compile-checked file that exercises every statement family: arithmetic and bitwise binds, string constants, memory reveal and scribe, immediate stores, stack operations, every conditional gate, labels, calls, returns, halt, nop and host calls. Its oracles are not named after server events, so loading the file does not run any of them.

## Configuration

```properties
scripts-enabled=true
script-dir=scripts
script-tick-interval-ms=1000
```

`on_tick` runs every `script-tick-interval-ms` milliseconds with a monotonically increasing counter in `$a0`. Set `scripts-enabled=false` to skip loading and execution entirely.

Operators can reload all scripts with `/script reload` and dispatch an oracle manually with `/script run <oracle>`. Reloading is synchronized with event execution.

## Operational notes

OBLVN has no file, network, or operating system access. Its only side effects are the host calls above. The instruction limit, memory limit, and stack limit are fixed at compile time. Scripts are trusted configuration, so keep the script directory writable only by the server administrator.
