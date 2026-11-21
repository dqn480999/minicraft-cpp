# OBLVN

OBLVN is the low-level register language embedded in the Minicraft C++ server. Source files use the `.obl` extension and live in `scripts/`. The compiler emits compact register bytecode; the virtual machine runs that bytecode inside the server under a script lock.

The syntax is intentionally ornate. There is no expression precedence, no implicit type conversion, no garbage collector and no comments. Every register, label, memory access and control transfer is written out.

## Execution model

Each script file is compiled once at startup or on `/script reload`. One file may declare one `ARCANA` and any number of `ORACLE` functions. Oracles named after server events are dispatched by the server. Other oracles are called with `CALL` or manually with `/script run <oracle>`.

Every invocation:

- resets all registers to zero;
- resets the data stack, call stack, comparison state and flags;
- copies event arguments into `$a0` through `$a3`;
- keeps the file's 4096-cell memory intact between invocations;
- stops after the configured instruction limit.

Memory is shared by all oracles in the same file and survives between events until the file is reloaded. Reloading clears it.

## File shape

```obl
ARCANA "module-name" {
    ORACLE @oracle_name ~> {
        <statements>
    }
}
```

`ARCANA` is a file group. `ORACLE` is a function. Oracle names must be unique inside a file. Labels are local to one oracle.

## Lexical rules

- Keywords are uppercase: `ARCANA`, `ORACLE`, `BIND`, `REVEAL`, `REVEAL8`, `SCRIBE`, `SCRIBE8`, `VERIFY`, `GATE`, `WHEN`, `CALL`, `RETURN`, `PUSH`, `POP`, `NEGATE`, `COMPLEMENT`, `INCREMENT`, `DECREMENT`, `SWAP`, `SUMMON`, `TRAP`, `HALT`, `NOP`, `SIGIL`, `MEM`, `MEM8`.
- Registers are `$r0` through `$r11` and `$a0` through `$a3`.
- Labels are `@name`; a definition is `@name::`.
- Integers are `#42`, `#-7`, `#0`.
- Strings are `"text"` with `\\`, `\"`, `\n`, `\t` and `\r` escapes.
- Comments do not exist.
- Statements end with `;`.

## Grammar

```text
program     = { arcana } ;
arcana      = "ARCANA" string "{" { oracle } "}" ;
oracle      = "ORACLE" label "~>" "{" { statement } "}" ;

statement   = labeldef | bind | reveal | reveal8 | scribe | scribe8
            | unary | swap | verify | gate | when | call | ret
            | push | pop | summon | trap | halt | nop ;

labeldef    = label "::" ;
bind        = "BIND" "(" register ")" "::=" operand [ binary operator operand ] ";" ;
reveal      = "REVEAL" "(" register ")" "::=" "MEM" address ";" ;
reveal8     = "REVEAL8" "(" register ")" "::=" "MEM8" address ";" ;
scribe      = "SCRIBE" "MEM" address "::=" operand ";" ;
scribe8     = "SCRIBE8" "MEM8" address "::=" operand ";" ;
unary       = ( "NEGATE" | "COMPLEMENT" | "INCREMENT" | "DECREMENT" )
              "(" register ")" ";" ;
swap        = "SWAP" "(" register "," register ")" ";" ;
verify      = "VERIFY" "(" register ")" "<=>" operand ";" ;
gate        = "GATE" label ";" ;
when        = "WHEN" "::" condition "=>" "GATE" label ";" ;
call        = "CALL" label ";" ;
ret         = "RETURN" ";" ;
push        = "PUSH" "(" register ")" ";" ;
pop         = "POP" "(" register ")" ";" ;
summon      = "SUMMON" "(" "#" integer ")" ";" ;
trap        = "TRAP" "(" "#" integer ")" ";" ;
halt        = "HALT" ";" ;
nop         = "NOP" ";" ;

address     = "[" register [ "+" ( "#" integer | register ) ] "]" ;
operand     = register | "#" integer | "SIGIL" "(" string ")" ;
binary      = "<+>" | "<->" | "<*>" | "</>" | "<%>"
            | "<&>" | "<|>" | "<^>" | "<<>" | "<>>" ;
condition   = "EQUAL" | "NOT_EQUAL" | "LESS" | "GREATER"
            | "LESS_OR_EQUAL" | "LESS_EQUAL"
            | "GREATER_OR_EQUAL" | "GREATER_EQUAL"
            | "ZERO" | "NONZERO" | "CARRY" | "NO_CARRY"
            | "OVERFLOW" | "NO_OVERFLOW" ;
```

## Registers

`$r0` through `$r11` are general 64-bit signed registers. `$a0` through `$a3` are registers 12 through 15 and carry event arguments. A host call reads `$a0` through `$a3` and writes its result to `$r0`.

All integer arithmetic is 64-bit and wraps on overflow. Signed division has one defined corner case: `INT64_MIN / -1` returns `INT64_MIN`, and the matching remainder is `0`.

## Memory

Memory is a flat array of 4096 signed 64-bit cells.

- `REVEAL` and `SCRIBE` read and write a full 64-bit cell.
- `REVEAL8` and `SCRIBE8` read and write the low 8 bits. A byte read is zero-extended to 64 bits.
- An address is `base + offset + index`. `offset` is a compile-time integer; `index` is an optional register.
- Address arithmetic wraps as 64-bit unsigned and is then checked against `0..4095`. Invalid addresses trap.

Examples:

```obl
BIND($r0) ::= #0;
BIND($r1) ::= #8;
SCRIBE MEM[$r0 + #0] ::= #99;
SCRIBE MEM[$r0 + $r1] ::= #7;
REVEAL($r2) ::= MEM[$r0 + #0];
REVEAL($r3) ::= MEM[$r0 + $r1];
SCRIBE8 MEM8[$r0 + #16] ::= #300;
REVEAL8($r4) ::= MEM8[$r0 + #16];
```

After this sequence `$r2` is `99`, `$r3` is `7`, and `$r4` is `44`.

Memory persists between invocations of oracles in the same file. This makes counters and state machines possible without host support.

## Stacks and calls

The data stack used by `PUSH` and `POP` holds up to 256 values. The call stack used by `CALL` and `RETURN` holds up to 64 return addresses. They are separate, so `PUSH` cannot corrupt a return address.

`CALL` accepts a local label or an oracle in the same file. Cross-file calls are not supported.

`RETURN` exits the current call. A `RETURN` with an empty call stack exits the oracle successfully. `HALT` exits the whole oracle immediately.

## Flags

`VERIFY($a) <=> $b` subtracts `b` from `a`, stores a signed comparison result and updates the flags.

Arithmetic instructions update the zero, negative, carry and overflow flags. `MOV`, loads, stores, stack operations and host calls do not change flags.

| Flag | Meaning |
| --- | --- |
| zero | last arithmetic or comparison result is zero |
| negative | last arithmetic or comparison result is negative |
| carry | unsigned carry for add, unsigned borrow for subtract and compare |
| overflow | signed overflow for add, subtract, increment, decrement, negate and compare |

`EQUAL`, `NOT_EQUAL`, `LESS`, `GREATER`, `LESS_OR_EQUAL` and `GREATER_OR_EQUAL` use the last `VERIFY` comparison. `ZERO`, `NONZERO`, `CARRY`, `NO_CARRY`, `OVERFLOW` and `NO_OVERFLOW` use the flags.

## Statements

### Bind

```obl
BIND($r0) ::= #10;
BIND($r1) ::= $r0;
BIND($r2) ::= SIGIL("literal");
BIND($r0) ::= $r1 <+> $r2;
BIND($r0) ::= $r0 <+> #1;
BIND($r3) ::= $r3 </> #2;
BIND($r0) ::= $a0 <+> $a1;
```

The left side of a binary bind is a register. The right side may be a register, an integer or `SIGIL("...")`. `SIGIL` returns an id in the file's compile-time string table.

### Unary operations

```obl
NEGATE($r0);
COMPLEMENT($r0);
INCREMENT($r0);
DECREMENT($r0);
```

`NEGATE` computes `0 - value`. `COMPLEMENT` computes the 64-bit bitwise complement.

### Exchange

```obl
SWAP($r0, $r1);
```

### Memory

```obl
REVEAL($r2) ::= MEM[$r1 + #16];
REVEAL8($r2) ::= MEM8[$r1 + #16];
SCRIBE MEM[$r1 + #16] ::= $r2;
SCRIBE MEM[$r1 + #16] ::= #-1;
SCRIBE8 MEM8[$r1 + #16] ::= $r2;
```

### Verify and condition

```obl
VERIFY($r0) <=> #50;
WHEN::EQUAL => GATE @label;
WHEN::OVERFLOW => GATE @overflow;
```

### Labels and gates

```obl
@start::
GATE @start;
GATE @done;
@done::
```

### Calls

```obl
CALL @helper;
RETURN;
```

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

### Trap

```obl
TRAP(#99);
```

A trap logs an error and stops the current oracle.

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

Block state is a 16-bit value: block id shifted left by four, plus metadata.

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

Compile-time `SIGIL` ids belong to the file's string table. Runtime event strings use ids from 100000 upward. Host call 2 resolves both.

Every matching oracle in every loaded file runs. Events are serialized by the script lock.

## Opcode reference

| Opcode | Produced by | Meaning |
| --- | --- | --- |
| `MOV` | `BIND` | register or immediate move |
| `LOAD` | `REVEAL` | 64-bit memory read |
| `LOAD8` | `REVEAL8` | 8-bit zero-extended memory read |
| `STORE` | `SCRIBE` | 64-bit memory write |
| `STORE8` | `SCRIBE8` | 8-bit memory write |
| `ADD` | `<+>` | wrapping add |
| `SUB` | `<->` | wrapping subtract |
| `MUL` | `<*>` | wrapping multiply |
| `DIV` | `</>` | signed divide |
| `MOD` | `<%>` | signed remainder |
| `AND` | `<&>` | bitwise and |
| `OR` | `<\|>` | bitwise or |
| `XOR` | `<^>` | bitwise xor |
| `SHL` | `<<>` | shift left, count masked to 6 bits |
| `SHR` | `<>>` | arithmetic shift right, count masked to 6 bits |
| `NEG` | `NEGATE` | two's complement negate |
| `NOT` | `COMPLEMENT` | bitwise complement |
| `INC` | `INCREMENT` | add one |
| `DEC` | `DECREMENT` | subtract one |
| `SWAP` | `SWAP` | exchange two registers |
| `CMP` | `VERIFY` | signed compare and flag update |
| `JMP` | `GATE` | unconditional jump |
| `JE` | `EQUAL` | jump if comparison equal |
| `JNE` | `NOT_EQUAL` | jump if comparison not equal |
| `JL` | `LESS` | jump if comparison less |
| `JG` | `GREATER` | jump if comparison greater |
| `JLE` | `LESS_OR_EQUAL` | jump if less or equal |
| `JGE` | `GREATER_OR_EQUAL` | jump if greater or equal |
| `JZ` | `ZERO` | jump if zero flag set |
| `JNZ` | `NONZERO` | jump if zero flag clear |
| `JC` | `CARRY` | jump if carry flag set |
| `JNC` | `NO_CARRY` | jump if carry flag clear |
| `JO` | `OVERFLOW` | jump if overflow flag set |
| `JNO` | `NO_OVERFLOW` | jump if overflow flag clear |
| `CALL` | `CALL` | push return address and jump |
| `RET` | `RETURN` | return or exit oracle |
| `PUSH` | `PUSH` | push data value |
| `POP` | `POP` | pop data value |
| `HOST` | `SUMMON` | native host call |
| `TRAP` | `TRAP` | stop with an error code |
| `HALT` | `HALT` | stop the oracle |
| `NOP` | `NOP` | do nothing |

## Errors and limits

Compile errors report `file:line:column`. Runtime traps report the file, oracle, program counter and reason.

- 4096 memory cells.
- 256 data stack entries.
- 64 call stack entries.
- Default 200000 instructions per invocation; configurable.
- Division or remainder by zero traps.
- Out-of-range memory traps.
- Unknown host call, string id, oracle or label traps or fails compilation.
- A trap stops the current oracle, not the server.

## Debugging

```properties
debug=true
script-trace=true
script-disassemble=true
script-max-instructions=200000
```

`script-trace` logs every executed instruction at debug level. `script-disassemble` logs the compiled bytecode for each loaded file. Both use the normal debug log. `script-max-instructions` is clamped to `1000..10000000`.

Operators can reload scripts and dispatch oracles at runtime:

```text
/script reload
/script run <oracle>
```

## Configuration

```properties
scripts-enabled=true
script-dir=scripts
script-tick-interval-ms=1000
script-max-instructions=200000
script-trace=false
script-disassemble=false
```

`on_tick` runs every `script-tick-interval-ms` milliseconds with a monotonically increasing counter in `$a0`. Set `scripts-enabled=false` to skip loading and execution entirely.

## Complete examples

`scripts/welcome.obl` greets joining players and announces a broken torch:

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

`scripts/stats.obl` broadcasts the online player count once per minute:

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

A persistent join counter:

```obl
ARCANA "joins" {
    ORACLE @on_join ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= SIGIL("&eJoins so far:");
        SUMMON(#2);
        BIND($a0) ::= $r1;
        SUMMON(#1);
        RETURN;
    }
}
```

`scripts/syntax_test.obl` is a compile-checked showcase of every statement family: arithmetic, bitwise operations, unary operations, swap, string constants, word and byte memory, indirect addresses, stack operations, every conditional gate, labels, calls, returns, halt, nop, host calls and an explicit trap oracle. Its oracles are not event names, so loading does not run them.

## Operational notes

OBLVN has no file, network or operating system access. Its only side effects are the host calls above. Scripts are trusted configuration and should only be writable by the server administrator.
