# OBLVN 2 Language Reference

OBLVN is the low-level scripting language embedded in the Minicraft C++ server. It is a register machine with persistent word memory, byte memory, separate data and call stacks, explicit flags, conditional gates, native host calls and event oracles.

This document is the complete language reference: syntax, execution model, memory, flags, every statement, every opcode, host calls, events, errors, debugging, configuration, tutorials, recipes and operational guidance.

OBLVN source files use the `.obl` extension. They are compiled when the server starts and can be hot reloaded by an operator.

## Table of contents

1. Introduction
2. Design goals
3. Execution model
4. Loading and lifecycle
5. Lexical structure
6. Formal grammar
7. Values and registers
8. Memory model
9. Stacks and calls
10. Flags and comparisons
11. Statements reference
12. Conditions
13. Opcode reference
14. Host calls
15. Events
16. Error catalog
17. Debugging and disassembly
18. Configuration reference
19. Tutorial
20. Recipes
21. Patterns and performance
22. Security model
23. Migration notes
24. FAQ
25. Glossary
26. Appendix: complete examples
27. Bit-level cookbook
28. Low-level script library

## 1. Introduction

OBLVN is not a general-purpose application language. It is an embedded event language for a Minecraft server core. It exists to let administrators change small pieces of server behavior without rebuilding or restarting the server.

Typical uses:

- greet players when they join;
- announce block events;
- maintain counters in persistent memory;
- broadcast periodic statistics;
- react to commands;
- perform small world edits;
- validate and log server activity.

OBLVN is deliberately explicit. There is no expression precedence, no operator overloading, no implicit conversions, no automatic memory management, no closures and no comments. Every register, memory access and control transfer is written out.

A small OBLVN program looks like this:

```obl
ARCANA "welcome" {
    ORACLE @on_join ~> {
        BIND($a0) ::= SIGIL("&eWelcome to the obelisk");
        SUMMON(#2);
        HALT;
    }
}
```

The file declares one arcana, which is a compilation unit group. It declares one oracle, which is a function. The oracle is named `on_join`, so the server calls it after a player joins. The oracle loads a string id into argument register `$a0`, calls host function 2, which broadcasts a chat message, and halts.

## 2. Design goals

OBLVN 2 has the following goals.

### 2.1 Determinism

Given the same memory state, registers and arguments, an oracle produces the same side effects until a host call occurs. There is no hidden scheduler, no garbage collector and no background work inside the virtual machine.

### 2.2 Bounded execution

Every invocation has an instruction limit. Memory is a fixed 4096-cell array. The data stack and call stack have fixed limits. A malformed script cannot allocate unbounded memory or run forever without hitting a trap.

### 2.3 Explicit state

Registers are explicit. Memory addresses are explicit. Flags are explicit. There are no local variables, no scopes and no hidden temporaries. If a value must survive a call, it goes on the data stack or into memory.

### 2.4 Small host surface

Scripts cannot open files, open sockets, spawn processes or read environment variables. The only side effects are the documented host calls and the server events that invoke scripts.

### 2.5 Readable bytecode

The compiler emits a compact internal instruction stream. A disassembler can print it for debugging. The bytecode format is an internal in-memory representation, not a stable file format.

### 2.6 Deliberately ornate syntax

The syntax uses long uppercase keywords, sigils and bracketed operators. This is intentional. Scripts should look unlike ordinary application code and should make control flow and memory access obvious.

## 3. Execution model

### 3.1 Compilation units

A `.obl` file is parsed into one `Program`. A program contains:

- a vector of instructions;
- a vector of compile-time strings created by `SIGIL`;
- a map from oracle names to instruction offsets;
- a list of unresolved calls, if any;
- an origin string used in diagnostics.

Each loaded file becomes one independent program with its own memory array, string table and oracle map.

### 3.2 Oracles

An oracle is a function. It has a name and a body of statements. Oracles named after server events are dispatched automatically. Other oracles can be called from the same file or manually with `/script run <oracle>`.

Oracle names must be unique inside one file. Two different files may both define `on_tick`; both will run when the event fires.

### 3.3 Invocation

An invocation begins when an event fires or an operator dispatches an oracle. The virtual machine performs these steps:

1. Locate the oracle entry point.
2. Clear all 16 registers to zero.
3. Clear the data stack.
4. Clear the call stack.
5. Clear comparison state and flags.
6. Copy up to four arguments into `$a0` through `$a3`.
7. Load runtime string ids, if the event supplies strings.
8. Execute instructions until `HALT`, a top-level `RETURN`, a trap or the instruction limit.

Memory is not cleared. It belongs to the file and persists across invocations.

### 3.4 Arguments

Arguments are 64-bit signed integers. Event strings are passed as runtime string ids from 100000 upward. The first runtime string becomes `$a0`, the second becomes `$a1`, and so on. Numeric arguments follow the runtime strings.

For example, `on_chat` supplies the player name string and the message string. The VM receives `$a0` as the player name id, `$a1` as the message id, and `$a2` as the entity id.

### 3.5 Result registers

Host calls may write a result to `$r0`. Host calls read `$a0` through `$a3`. Other registers are not written by host calls, but scripts should not rely on host calls preserving flags unless they explicitly do.

### 3.6 Serialization

All script execution is serialized by a recursive mutex. A long-running script delays other scripts and server event dispatch that needs the same lock. The instruction limit is the primary protection against this.

### 3.7 Event observation

Most event oracles run after the server has already performed the action. For example, `on_block_break` runs after the block has been removed, and `on_chat` runs after the message has been broadcast. Scripts observe events; they do not cancel them. There is no cancellation API.

## 4. Loading and lifecycle

### 4.1 Directory scan

At startup, if `scripts-enabled=true`, the server scans `script-dir` for files ending in `.obl`. Files are sorted by path before compilation. Subdirectories are not scanned.

### 4.2 Compilation

Each file is compiled independently. A syntax error in one file is logged and that file is skipped. Other files continue to load. A call to an oracle that is not defined in the same file is a load error for that file.

### 4.3 Memory initialization

A newly loaded file receives a zeroed 4096-cell memory array. Memory stays attached to the file until reload or shutdown.

### 4.4 Reload

`/script reload` clears all loaded programs, all memories and all function counts, then recompiles the script directory. Reload is synchronized with event execution. Persistent counters are reset by reload.

### 4.5 Manual dispatch

`/script run <oracle>` fires the named oracle in every loaded file that defines it. The oracle receives no arguments and no runtime strings, so `$a0` through `$a3` are zero and no runtime string ids exist.

### 4.6 Shutdown

Scripts do not receive a shutdown event. Server shutdown stops the tick thread and network layer. Persistent memory is lost when the process exits.

## 5. Lexical structure

### 5.1 Whitespace

Spaces, tabs, carriage returns and newlines separate tokens. They are otherwise ignored.

### 5.2 Keywords

Keywords are uppercase and case-sensitive:

```text
ARCANA ORACLE BIND REVEAL REVEAL8 SCRIBE SCRIBE8 VERIFY GATE WHEN
CALL RETURN PUSH POP NEGATE COMPLEMENT INCREMENT DECREMENT SWAP
POPCOUNT LEADING_ZEROS TRAILING_ZEROS BYTESWAP CLZ CTZ BSWAP
SUMMON TRAP HALT NOP SIGIL MEM MEM8
```

Using a keyword in lowercase is a parse error unless it is a label or register name.

### 5.3 Identifiers

An identifier starts with an ASCII letter or underscore and continues with ASCII letters, digits or underscores. Oracle names and labels use identifiers.

### 5.4 Registers

Registers are written with a dollar sign:

```text
$r0 $r1 $r2 $r3 $r4 $r5 $r6 $r7 $r8 $r9 $r10 $r11
$a0 $a1 $a2 $a3
```

`$a0` through `$a3` are aliases for registers 12 through 15. `$r12` and above are not valid syntax.

### 5.5 Labels

A label reference is written `@name`. A label definition is written `@name::`. Labels are local to one oracle.

### 5.6 Integers

Integers are written with a hash sign:

```text
#0
#42
#-7
#9223372036854775807
```

The valid range is the signed 64-bit range. A number that does not fit is a compile error.

### 5.7 Strings

Strings are written in double quotes:

```text
"hello"
"line one\nline two"
"a quote: \""
"a backslash: \\"
```

Supported escapes:

| Escape | Meaning |
| --- | --- |
| `\\` | backslash |
| `\"` | double quote |
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |

Unknown escapes are compile errors. Unterminated strings are compile errors.

### 5.8 Operators and punctuation

```text
::= <=> ~> => :: <+> <-> <*> </> <%> <&> <|> <^> <<> <>>
{ } ( ) [ ] ; , +
```

The lexer matches multi-character symbols before single-character symbols.

### 5.9 Comments

OBLVN has no comments. There is no line comment syntax and no block comment syntax. Documentation belongs in separate files, not in executable source.

## 6. Formal grammar

The grammar is shown in a compact EBNF-like form.

```text
program     = { arcana } ;
arcana      = "ARCANA" string "{" { oracle } "}" ;
oracle      = "ORACLE" label "~>" "{" { statement } "}" ;

statement   = labeldef
            | bind
            | reveal
            | reveal8
            | scribe
            | scribe8
            | unary
            | swap
            | verify
            | gate
            | when
            | call
            | ret
            | push
            | pop
            | summon
            | trap
            | halt
            | nop ;

labeldef    = label "::" ;
bind        = "BIND" "(" register ")" "::=" operand
              [ binary operand ] ";" ;
reveal      = "REVEAL" "(" register ")" "::=" "MEM" address ";" ;
reveal8     = "REVEAL8" "(" register ")" "::=" "MEM8" address ";" ;
scribe      = "SCRIBE" "MEM" address "::=" operand ";" ;
scribe8     = "SCRIBE8" "MEM8" address "::=" operand ";" ;

unary       = ( "NEGATE" | "COMPLEMENT" | "INCREMENT" | "DECREMENT"
              | "POPCOUNT" | "LEADING_ZEROS" | "TRAILING_ZEROS"
              | "BYTESWAP" | "CLZ" | "CTZ" | "BSWAP" )
              "(" register ")" ";" ;
swap        = "SWAP" "(" register "," register ")" ";" ;
verify      = "VERIFY" "(" register ")" "<=>" operand ";" ;
gate        = "GATE" label ";" ;
when        = "WHEN" "::" condition "=>" "GATE" label ";" ;
call        = "CALL" label ";" ;
ret         = "RETURN" ";" ;
push        = "PUSH" "(" register ")" ";" ;
pop         = "POP" "(" register ")" ";" ;
summon      = "SUMMON" "(" number ")" ";" ;
trap        = "TRAP" "(" number ")" ";" ;
halt        = "HALT" ";" ;
nop         = "NOP" ";" ;

address     = "[" register [ "+" ( number | register ) ] "]" ;
operand     = register | number | "SIGIL" "(" string ")" ;
binary      = "<+>" | "<->" | "<*>" | "</>" | "<%>"
            | "<&>" | "<|>" | "<^>" | "<<>" | "<>>"
            | "<rol>" | "<ror>" ;

condition   = "EQUAL" | "NOT_EQUAL" | "LESS" | "GREATER"
            | "LESS_OR_EQUAL" | "LESS_EQUAL"
            | "GREATER_OR_EQUAL" | "GREATER_EQUAL"
            | "ZERO" | "NONZERO" | "CARRY" | "NO_CARRY"
            | "OVERFLOW" | "NO_OVERFLOW" ;

register    = "$r" digit [ digit ] | "$a" digit ;
label       = "@" identifier ;
number      = "#" [ "-" ] digit { digit } ;
string      = '"' { character } '"' ;
```

A file may contain more than one `ARCANA` block. Oracle names must be unique across all arcana blocks in the same file.

## 7. Values and registers

### 7.1 Value type

Every value is a signed 64-bit integer. There are no floats, booleans, chars, arrays or objects at the language level.

Booleans are represented by integers: zero is false and non-zero is true.

### 7.2 Wrapping arithmetic

Add, subtract, multiply, increment, decrement and negate wrap modulo 2^64. Overflow is visible through the overflow flag and carry is visible through the carry flag.

### 7.3 Register map

| Register | Role |
| --- | --- |
| `$r0` | general register; host result |
| `$r1` | general register |
| `$r2` | general register |
| `$r3` | general register |
| `$r4` | general register |
| `$r5` | general register |
| `$r6` | general register |
| `$r7` | general register |
| `$r8` | general register |
| `$r9` | general register |
| `$r10` | general register |
| `$r11` | general register |
| `$a0` | argument 0, alias of register 12 |
| `$a1` | argument 1, alias of register 13 |
| `$a2` | argument 2, alias of register 14 |
| `$a3` | argument 3, alias of register 15 |

### 7.4 Register lifetime

Registers are reset to zero at the start of every invocation. They are not persistent. Use memory for values that must survive between invocations.

### 7.5 Argument registers

Event dispatch writes event arguments into `$a0` through `$a3`. A script may overwrite them. Host calls read them at the moment of the call.

### 7.6 Return register

Host calls that return a value write it to `$r0`. A script that needs the result must copy it before calling another host function or before making another call that might reuse `$r0`.

## 8. Memory model

### 8.1 Cell count

Memory contains exactly 4096 cells, numbered 0 through 4095. Each cell holds a signed 64-bit integer.

### 8.2 Word access

`REVEAL` and `SCRIBE` read and write a full 64-bit cell.

```obl
REVEAL($r1) ::= MEM[$r0 + #16];
SCRIBE MEM[$r0 + #16] ::= $r1;
SCRIBE MEM[$r0 + #16] ::= #-1;
```

### 8.3 Byte access

`REVEAL8` and `SCRIBE8` operate on the low 8 bits of a cell.

```obl
SCRIBE8 MEM8[$r0 + #4] ::= #300;
REVEAL8($r1) ::= MEM8[$r0 + #4];
```

After this, the cell contains 44 and `$r1` contains 44. A byte read is zero-extended. A byte write clears the upper bits of the cell.

### 8.4 Address calculation

An address is computed as:

```text
address = base + offset + index
```

- `base` is always a register.
- `offset` is an optional compile-time integer.
- `index` is an optional register.

All address arithmetic is 64-bit wrapping arithmetic. The final value is checked against the range 0 through 4095. An out-of-range address traps.

Examples:

```obl
BIND($r0) ::= #0;
BIND($r1) ::= #8;
REVEAL($r2) ::= MEM[$r0 + #0];
REVEAL($r3) ::= MEM[$r0 + $r1];
SCRIBE MEM[$r0] ::= #42;
SCRIBE MEM[$r0 + #4] ::= #43;
SCRIBE MEM[$r0 + $r1] ::= #44;
```

### 8.5 Persistence

Memory belongs to the file. Every oracle in the same file sees the same memory. Memory persists across invocations and across events. It is cleared by `/script reload` and by server restart.

This makes memory suitable for counters, flags, timestamps and small state machines.

### 8.6 No type tags

Memory cells do not store type information. A cell written as a word can later be read as a byte. A cell written as a byte has its upper bits cleared.

### 8.7 No automatic initialization between files

Each file has its own independent memory. Two files with the same oracle names do not share counters.

### 8.8 String tables are not in memory

Compile-time strings are stored in a separate string table. `SIGIL` returns an id, not a memory address. Runtime strings also use ids. Host call 2 resolves ids against both tables.

### 8.9 Bounds safety

There is no pointer arithmetic outside the memory array. An invalid address stops the current oracle with a runtime trap. The server stays alive.

## 9. Stacks and calls

### 9.1 Data stack

`PUSH($r)` pushes a value onto the data stack. `POP($r)` pops a value into a register. The data stack holds up to 256 values.

```obl
PUSH($r1);
PUSH($r2);
POP($r3);
POP($r4);
```

The last value pushed is the first value popped.

### 9.2 Call stack

`CALL @label` pushes the return address onto a separate call stack and jumps. `RETURN` pops the call stack and returns. The call stack holds up to 64 addresses.

The data stack and call stack are separate. Pushing data cannot corrupt a return address.

### 9.3 Empty call stack

If `RETURN` executes with an empty call stack, it exits the oracle successfully. This lets the same oracle be used both as an event handler and as a called helper.

### 9.4 HALT

`HALT` exits the whole oracle immediately, regardless of the call stack. It is not a return.

### 9.5 Call targets

`CALL` accepts a local label or an oracle name in the same file. Calls across files are not supported. If the name is not a local label, it is resolved against the file's oracle map during loading.

### 9.6 Recursion

Recursion is allowed until the call stack limit is reached. Deep recursion traps with a call stack overflow error.

## 10. Flags and comparisons

### 10.1 Comparison state

`VERIFY($a) <=> $b` performs a signed comparison of `$a` and `$b`. It records one of three comparison states:

- less than;
- equal;
- greater than.

The comparison state is used by `EQUAL`, `NOT_EQUAL`, `LESS`, `GREATER`, `LESS_OR_EQUAL` and `GREATER_OR_EQUAL`.

`VERIFY` does not write a register. It updates flags.

### 10.2 Zero flag

The zero flag is set when the last arithmetic or comparison result is zero. It is cleared otherwise.

`ZERO` and `NONZERO` use the zero flag. They are useful after arithmetic:

```obl
BIND($r0) ::= $a0 <&> #1;
WHEN::ZERO => GATE @even;
GATE @odd;
@even::
HALT;
@odd::
HALT;
```

### 10.3 Negative flag

The negative flag is set when the last arithmetic or comparison result is negative in two's complement.

There is no direct `NEGATIVE` condition in OBLVN 2. Use `VERIFY` with zero:

```obl
VERIFY($r0) <=> #0;
WHEN::LESS => GATE @negative;
```

### 10.4 Carry flag

Carry has two meanings depending on the operation:

- for add, increment and negate-related addition, it is the unsigned carry out;
- for subtract, decrement, compare and negate, it is the unsigned borrow.

`CARRY` jumps when the flag is set. `NO_CARRY` jumps when it is clear.

### 10.5 Overflow flag

Overflow is signed overflow. It is set by add, subtract, increment, decrement, negate and compare when the mathematical result does not fit in 64 bits.

`OVERFLOW` jumps when the flag is set. `NO_OVERFLOW` jumps when it is clear.

### 10.6 Which instructions update flags

Flags are updated by:

- `ADD`;
- `SUB`;
- `MUL`;
- `DIV`;
- `MOD`;
- `AND`;
- `OR`;
- `XOR`;
- `SHL`;
- `SHR`;
- `NEG`;
- `NOT`;
- `INC`;
- `DEC`;
- `CMP`.

Multiply, divide, modulo, bitwise operations and shifts clear carry and overflow and update zero and negative.

Flags are not updated by:

- `MOV`;
- `LOAD`;
- `LOAD8`;
- `STORE`;
- `STORE8`;
- `SWAP`;
- `PUSH`;
- `POP`;
- `CALL`;
- `RETURN`;
- `HOST`;
- `HALT`;
- `NOP`;
- jumps.

### 10.7 Comparison state lifetime

Comparison state persists until the next `VERIFY`. Arithmetic does not update comparison state. After arithmetic, use `ZERO`/`NONZERO` or execute a new `VERIFY` before using `EQUAL`/`LESS`/`GREATER`.

This is a deliberate low-level behavior. It prevents arithmetic from silently changing what a later `WHEN::LESS` means.

### 10.8 Examples

Unsigned carry after addition:

```obl
BIND($r0) ::= #-1;
BIND($r1) ::= #1;
BIND($r2) ::= $r0 <+> $r1;
WHEN::CARRY => GATE @carried;
```

Signed overflow after addition:

```obl
BIND($r0) ::= #9223372036854775807;
INCREMENT($r0);
WHEN::OVERFLOW => GATE @overflowed;
```

Equality after comparison:

```obl
VERIFY($a0) <=> #50;
WHEN::EQUAL => GATE @torch;
```

## 11. Statements reference

This section documents every statement form.

### 11.1 BIND

Syntax:

```obl
BIND($dst) ::= operand;
BIND($dst) ::= lhs binary rhs;
```

Move form:

```obl
BIND($r0) ::= #10;
BIND($r1) ::= $r0;
BIND($r2) ::= SIGIL("literal");
```

The move form copies a register, an integer or a string id into the destination register.

Binary form:

```obl
BIND($r0) ::= $r1 <+> #1;
BIND($r3) ::= $r3 </> #2;
BIND($r0) ::= $a0 <+> $a1;
```

Semantics:

1. The left operand is materialized in the destination register.
2. The binary operation is applied using the destination register as the left side.
3. The result is written to the destination register.

This means the left operand may be a register, an integer or a string id. Using a register as the left operand is the most common form.

Operators:

| Operator | Operation |
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
| `<>>` | arithmetic shift right |
| `<rol>` | rotate left |
| `<ror>` | rotate right |

Division and remainder by zero trap.

Shifts and rotates mask the count to 6 bits. Shifting or rotating by 64 is the same as by 0.

### 11.2 REVEAL

Syntax:

```obl
REVEAL($dst) ::= MEM[address];
```

Reads one 64-bit cell into a register. Out-of-range addresses trap.

### 11.3 REVEAL8

Syntax:

```obl
REVEAL8($dst) ::= MEM8[address];
```

Reads the low 8 bits of one cell and zero-extends them into a register. Out-of-range addresses trap.

### 11.4 SCRIBE

Syntax:

```obl
SCRIBE MEM[address] ::= $src;
SCRIBE MEM[address] ::= #value;
SCRIBE MEM[address] ::= SIGIL("value");
```

Writes a 64-bit value into a cell. Out-of-range addresses trap.

### 11.5 SCRIBE8

Syntax:

```obl
SCRIBE8 MEM8[address] ::= $src;
SCRIBE8 MEM8[address] ::= #value;
```

Writes the low 8 bits of a value into a cell. The upper 56 bits become zero. Out-of-range addresses trap.

### 11.6 VERIFY

Syntax:

```obl
VERIFY($register) <=> operand;
```

Compares a register with a register, an integer or a string id. Updates comparison state and flags. Does not write a register.

### 11.7 GATE

Syntax:

```obl
GATE @label;
```

Unconditional jump to a label in the same oracle. The label may be forward or backward.

### 11.8 WHEN

Syntax:

```obl
WHEN::condition => GATE @label;
```

Conditional jump to a label in the same oracle. The condition is evaluated against the current comparison state or flags.

### 11.9 Label definition

Syntax:

```obl
@name::
```

Defines a label at the current instruction offset. Labels are local to one oracle. Duplicate labels in one oracle are compile errors.

### 11.10 CALL

Syntax:

```obl
CALL @target;
```

Pushes the return address onto the call stack and jumps to the target. The target may be a local label or an oracle in the same file.

### 11.11 RETURN

Syntax:

```obl
RETURN;
```

Pops the call stack and returns to the caller. If the call stack is empty, the oracle exits successfully.

### 11.12 PUSH

Syntax:

```obl
PUSH($register);
```

Pushes a value onto the data stack. The data stack limit is 256 entries.

### 11.13 POP

Syntax:

```obl
POP($register);
```

Pops a value from the data stack into a register. An empty data stack traps.

### 11.14 NEGATE

Syntax:

```obl
NEGATE($register);
```

Computes two's complement negation. The result wraps.

### 11.15 COMPLEMENT

Syntax:

```obl
COMPLEMENT($register);
```

Computes the bitwise complement of a register.

### 11.16 INCREMENT

Syntax:

```obl
INCREMENT($register);
```

Adds one. The result wraps and updates flags.

### 11.17 DECREMENT

Syntax:

```obl
DECREMENT($register);
```

Subtracts one. The result wraps and updates flags.

### 11.18 SWAP

Syntax:

```obl
SWAP($left, $right);
```

Exchanges two registers.

### 11.18.1 POPCOUNT

Syntax:

```obl
POPCOUNT($register);
```

Replaces the register with the number of set bits in its 64-bit value. Updates zero and negative flags; clears carry and overflow.

### 11.18.2 LEADING_ZEROS

Syntax:

```obl
LEADING_ZEROS($register);
```

Replaces the register with the number of leading zero bits. For zero the result is 64. `CLZ` is an alias.

### 11.18.3 TRAILING_ZEROS

Syntax:

```obl
TRAILING_ZEROS($register);
```

Replaces the register with the number of trailing zero bits. For zero the result is 64. `CTZ` is an alias.

### 11.18.4 BYTESWAP

Syntax:

```obl
BYTESWAP($register);
```

Reverses the byte order of the 64-bit value. `BSWAP` is an alias.

For example, input `0x0000000000000100` becomes `0x0001000000000000`.

### 11.19 SUMMON

Syntax:

```obl
SUMMON(#id);
```

Invokes a native host function. Arguments are read from `$a0` through `$a3`. A result may be written to `$r0`.

### 11.20 TRAP

Syntax:

```obl
TRAP(#code);
```

Stops the current oracle with a runtime trap and logs the code. This is useful for deliberate defensive branches.

### 11.21 HALT

Syntax:

```obl
HALT;
```

Stops the whole oracle immediately.

### 11.22 NOP

Syntax:

```obl
NOP;
```

Does nothing. Useful as a placeholder or as a label body.

### 11.23 SIGIL

Syntax as an operand:

```obl
BIND($r0) ::= SIGIL("text");
```

Adds the string to the file's compile-time string table and returns its id. The same string may be added more than once; no deduplication is performed.

### 11.24 MEM and MEM8

`MEM` marks a 64-bit memory operand. `MEM8` marks an 8-bit memory operand. Both are followed by a bracketed address.

### 11.25 Statement ordering

Statements execute in source order. Labels do not generate instructions. Jumps and calls redirect execution. There is no implicit control flow.

## 12. Conditions

### 12.1 Condition table

| Condition | Opcode | Tests |
| --- | --- | --- |
| `EQUAL` | `JE` | comparison state is equal |
| `NOT_EQUAL` | `JNE` | comparison state is not equal |
| `LESS` | `JL` | comparison state is less |
| `GREATER` | `JG` | comparison state is greater |
| `LESS_OR_EQUAL` | `JLE` | comparison state is less or equal |
| `LESS_EQUAL` | `JLE` | alias for `LESS_OR_EQUAL` |
| `GREATER_OR_EQUAL` | `JGE` | comparison state is greater or equal |
| `GREATER_EQUAL` | `JGE` | alias for `GREATER_OR_EQUAL` |
| `ZERO` | `JZ` | zero flag is set |
| `NONZERO` | `JNZ` | zero flag is clear |
| `CARRY` | `JC` | carry flag is set |
| `NO_CARRY` | `JNC` | carry flag is clear |
| `OVERFLOW` | `JO` | overflow flag is set |
| `NO_OVERFLOW` | `JNO` | overflow flag is clear |

### 12.2 Comparison conditions

The comparison conditions use the state recorded by the most recent `VERIFY`. They do not read the zero, carry or overflow flags directly. This separation is intentional:

```obl
VERIFY($r0) <=> $r1;
WHEN::LESS => GATE @less;
WHEN::GREATER => GATE @greater;
WHEN::EQUAL => GATE @equal;
```

After `VERIFY`, exactly one of less, equal or greater is true.

`LESS_OR_EQUAL` and `GREATER_OR_EQUAL` combine two comparison states:

```obl
VERIFY($a0) <=> #10;
WHEN::LESS_OR_EQUAL => GATE @small;
```

### 12.3 Flag conditions

The flag conditions use the arithmetic flags:

```obl
INCREMENT($r0);
WHEN::OVERFLOW => GATE @wrapped;
```

```obl
BIND($r0) ::= #-1;
BIND($r1) ::= #1;
BIND($r2) ::= $r0 <+> $r1;
WHEN::CARRY => GATE @carried;
```

### 12.4 Zero flag after comparison

`VERIFY` also updates the zero flag. Therefore both forms are equivalent immediately after `VERIFY`:

```obl
VERIFY($r0) <=> #0;
WHEN::EQUAL => GATE @zero;
```

```obl
VERIFY($r0) <=> #0;
WHEN::ZERO => GATE @zero;
```

They are not equivalent after arithmetic. `EQUAL` still uses the old comparison state, while `ZERO` uses the arithmetic result.

### 12.5 Condition pitfalls

The most common mistake is using `EQUAL` after arithmetic without a fresh `VERIFY`:

```obl
BIND($r0) ::= #5;
BIND($r0) ::= $r0 <-> #5;
WHEN::EQUAL => GATE @wrong;
```

`EQUAL` here reads a comparison state that may be stale. Use:

```obl
BIND($r0) ::= #5;
BIND($r0) ::= $r0 <-> #5;
WHEN::ZERO => GATE @right;
```

### 12.6 Nested conditions

There is no `if` statement. Combine conditions with gates:

```obl
VERIFY($r0) <=> #0;
WHEN::LESS => GATE @negative;
VERIFY($r0) <=> #100;
WHEN::GREATER => GATE @large;
GATE @normal;
@negative::
GATE @done;
@large::
GATE @done;
@normal::
GATE @done;
@done::
HALT;
```

## 13. Opcode reference

This section documents the internal instruction set. Most users write statements, not opcodes, but the opcode model explains the runtime precisely and makes disassembly readable.

### 13.1 Instruction fields

Every instruction contains:

| Field | Meaning |
| --- | --- |
| `op` | opcode |
| `dst` | primary register operand |
| `src` | secondary register operand or `kImmediate` |
| `index` | optional index register or `kNoIndex` |
| `imm` | primary immediate or jump target |
| `imm2` | secondary immediate, used by immediate stores |
| `line` | source line |
| `column` | source column |

The bytecode format is an in-memory representation. OBLVN does not define a portable serialized bytecode file format.

### 13.2 Register encoding

Registers 0 through 11 are `$r0` through `$r11`. Registers 12 through 15 are `$a0` through `$a3`.

Special operand markers:

| Marker | Meaning |
| --- | --- |
| `kImmediate` | `src` is not a register; use `imm` or `imm2` |
| `kNoIndex` | no index register |

### 13.3 Quick opcode table

| Opcode | Category |
| --- | --- |
| `MOV` | data movement |
| `LOAD` | word memory read |
| `LOAD8` | byte memory read |
| `STORE` | word memory write |
| `STORE8` | byte memory write |
| `ADD` | arithmetic |
| `SUB` | arithmetic |
| `MUL` | arithmetic |
| `DIV` | arithmetic |
| `MOD` | arithmetic |
| `AND` | bitwise |
| `OR` | bitwise |
| `XOR` | bitwise |
| `SHL` | shift |
| `SHR` | shift |
| `ROL` | rotate |
| `ROR` | rotate |
| `POPCOUNT` | bit inspection |
| `CLZ` | bit inspection |
| `CTZ` | bit inspection |
| `BSWAP` | byte order |
| `NEG` | unary arithmetic |
| `NOT` | unary bitwise |
| `INC` | unary arithmetic |
| `DEC` | unary arithmetic |
| `SWAP` | data movement |
| `CMP` | comparison |
| `JMP` | control flow |
| `JE` | conditional control flow |
| `JNE` | conditional control flow |
| `JL` | conditional control flow |
| `JG` | conditional control flow |
| `JLE` | conditional control flow |
| `JGE` | conditional control flow |
| `JZ` | conditional control flow |
| `JNZ` | conditional control flow |
| `JC` | conditional control flow |
| `JNC` | conditional control flow |
| `JO` | conditional control flow |
| `JNO` | conditional control flow |
| `CALL` | control flow |
| `RET` | control flow |
| `PUSH` | stack |
| `POP` | stack |
| `HOST` | native call |
| `TRAP` | fault |
| `HALT` | termination |
| `NOP` | no operation |

### 13.4 MOV

Syntax source: `BIND($dst) ::= operand;`

Semantics:

```text
if operand is register:
    reg[dst] = reg[src]
else:
    reg[dst] = imm
```

Flags are unchanged.

### 13.5 LOAD

Syntax source: `REVEAL($dst) ::= MEM[address];`

Semantics:

```text
address = reg[base] + offset + reg[index]
if address not in 0..4095:
    trap
reg[dst] = memory[address]
```

Flags are unchanged.

### 13.6 LOAD8

Syntax source: `REVEAL8($dst) ::= MEM8[address];`

Semantics:

```text
address = reg[base] + offset + reg[index]
if address not in 0..4095:
    trap
reg[dst] = memory[address] & 0xFF
```

The result is zero-extended. Flags are unchanged.

### 13.7 STORE

Syntax source: `SCRIBE MEM[address] ::= operand;`

Semantics:

```text
address = reg[base] + offset + reg[index]
if address not in 0..4095:
    trap
if operand is register:
    memory[address] = reg[src]
else:
    memory[address] = imm2
```

Flags are unchanged.

### 13.8 STORE8

Syntax source: `SCRIBE8 MEM8[address] ::= operand;`

Semantics:

```text
address = reg[base] + offset + reg[index]
if address not in 0..4095:
    trap
memory[address] = value & 0xFF
```

The upper 56 bits of the cell are cleared. Flags are unchanged.

### 13.9 ADD

Syntax source: `<+>`.

Semantics:

```text
result = wrapping_add(reg[dst], rhs)
reg[dst] = result
zero = result == 0
negative = result < 0
carry = unsigned_carry_add(lhs, rhs)
overflow = signed_overflow_add(lhs, rhs, result)
```

The comparison state is not changed.

### 13.10 SUB

Syntax source: `<->`.

Semantics:

```text
result = wrapping_sub(reg[dst], rhs)
reg[dst] = result
zero = result == 0
negative = result < 0
carry = unsigned_borrow(lhs, rhs)
overflow = signed_overflow_sub(lhs, rhs, result)
```

The comparison state is not changed.

### 13.11 MUL

Syntax source: `<*>`.

Semantics:

```text
result = wrapping_mul(reg[dst], rhs)
reg[dst] = result
zero = result == 0
negative = result < 0
carry = false
overflow = false
```

### 13.12 DIV

Syntax source: `</>`.

Semantics:

```text
if rhs == 0:
    trap
if lhs == INT64_MIN and rhs == -1:
    result = INT64_MIN
else:
    result = lhs / rhs
reg[dst] = result
zero = result == 0
negative = result < 0
carry = false
overflow = false
```

### 13.13 MOD

Syntax source: `<%>`.

Semantics:

```text
if rhs == 0:
    trap
if lhs == INT64_MIN and rhs == -1:
    result = 0
else:
    result = lhs % rhs
reg[dst] = result
zero = result == 0
negative = result < 0
carry = false
overflow = false
```

Remainder has the sign of the dividend.

### 13.14 AND, OR, XOR

Syntax source: `<&>`, `<|>`, `<^>`.

Semantics:

```text
result = lhs OP rhs
reg[dst] = result
zero = result == 0
negative = result < 0
carry = false
overflow = false
```

### 13.15 SHL

Syntax source: `<<>`.

Semantics:

```text
shift = rhs & 63
result = (uint64)lhs << shift
reg[dst] = (int64)result
zero/negative updated
carry = false
overflow = false
```

### 13.16 SHR

Syntax source: `<>>`.

Semantics:

```text
shift = rhs & 63
result = lhs >> shift
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

On the supported Linux/GCC and Linux/Clang targets this is an arithmetic shift for signed values.

### 13.16.1 ROL

Syntax source: `<rol>`.

Semantics:

```text
shift = rhs & 63
result = rotl64((uint64)lhs, shift)
reg[dst] = (int64)result
zero/negative updated
carry = false
overflow = false
```

### 13.16.2 ROR

Syntax source: `<ror>`.

Semantics:

```text
shift = rhs & 63
result = rotr64((uint64)lhs, shift)
reg[dst] = (int64)result
zero/negative updated
carry = false
overflow = false
```

### 13.16.3 POPCOUNT

Syntax source: `POPCOUNT($r);`

Semantics:

```text
result = popcount64((uint64)reg[dst])
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

### 13.16.4 CLZ

Syntax source: `LEADING_ZEROS($r);` or `CLZ($r);`

Semantics:

```text
result = countl_zero((uint64)reg[dst])
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

For a zero input the result is 64.

### 13.16.5 CTZ

Syntax source: `TRAILING_ZEROS($r);` or `CTZ($r);`

Semantics:

```text
result = countr_zero((uint64)reg[dst])
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

For a zero input the result is 64.

### 13.16.6 BSWAP

Syntax source: `BYTESWAP($r);` or `BSWAP($r);`

Semantics:

```text
result = bswap64((uint64)reg[dst])
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

### 13.17 NEG

Syntax source: `NEGATE($r);`

Semantics:

```text
lhs = 0
rhs = reg[dst]
result = wrapping_sub(lhs, rhs)
reg[dst] = result
update zero, negative, carry, overflow as subtract
```

### 13.18 NOT

Syntax source: `COMPLEMENT($r);`

Semantics:

```text
result = ~reg[dst]
reg[dst] = result
zero/negative updated
carry = false
overflow = false
```

### 13.19 INC

Syntax source: `INCREMENT($r);`

Semantics:

```text
lhs = reg[dst]
rhs = 1
result = wrapping_add(lhs, rhs)
reg[dst] = result
update zero, negative, carry, overflow as add
```

### 13.20 DEC

Syntax source: `DECREMENT($r);`

Semantics:

```text
lhs = reg[dst]
rhs = 1
result = wrapping_sub(lhs, rhs)
reg[dst] = result
update zero, negative, carry, overflow as subtract
```

### 13.21 SWAP

Syntax source: `SWAP($a, $b);`

Semantics:

```text
tmp = reg[a]
reg[a] = reg[b]
reg[b] = tmp
```

Flags are unchanged.

### 13.22 CMP

Syntax source: `VERIFY($a) <=> rhs;`

Semantics:

```text
lhs = reg[dst]
result = wrapping_sub(lhs, rhs)
compare = sign(result)
zero = result == 0
negative = result < 0
carry = unsigned_borrow(lhs, rhs)
overflow = signed_overflow_sub(lhs, rhs, result)
```

`compare` is -1, 0 or 1.

### 13.23 JMP

Syntax source: `GATE @label;`

Semantics:

```text
pc = target
```

### 13.24 JE and JNE

Syntax source: `WHEN::EQUAL`, `WHEN::NOT_EQUAL`.

Semantics:

```text
if compare == 0: pc = target
```

for `JE`, and:

```text
if compare != 0: pc = target
```

for `JNE`.

### 13.25 JL, JG, JLE, JGE

Syntax source: `WHEN::LESS`, `WHEN::GREATER`, `WHEN::LESS_OR_EQUAL`, `WHEN::GREATER_OR_EQUAL`.

Semantics:

```text
JL:  if compare < 0
JG:  if compare > 0
JLE: if compare <= 0
JGE: if compare >= 0
```

### 13.26 JZ and JNZ

Syntax source: `WHEN::ZERO`, `WHEN::NONZERO`.

Semantics:

```text
JZ:  if zero
JNZ: if not zero
```

### 13.27 JC and JNC

Syntax source: `WHEN::CARRY`, `WHEN::NO_CARRY`.

Semantics:

```text
JC:  if carry
JNC: if not carry
```

### 13.28 JO and JNO

Syntax source: `WHEN::OVERFLOW`, `WHEN::NO_OVERFLOW`.

Semantics:

```text
JO:  if overflow
JNO: if not overflow
```

### 13.29 CALL

Syntax source: `CALL @target;`

Semantics:

```text
if call stack is full:
    trap
push pc onto call stack
pc = target
```

### 13.30 RET

Syntax source: `RETURN;`

Semantics:

```text
if call stack is empty:
    terminate oracle successfully
pc = pop call stack
```

### 13.31 PUSH

Syntax source: `PUSH($r);`

Semantics:

```text
if data stack is full:
    trap
push reg[dst]
```

### 13.32 POP

Syntax source: `POP($r);`

Semantics:

```text
if data stack is empty:
    trap
reg[dst] = pop data stack
```

### 13.33 HOST

Syntax source: `SUMMON(#id);`

Semantics depend on the host id. See the host call reference.

### 13.34 TRAP

Syntax source: `TRAP(#code);`

Semantics:

```text
raise runtime trap with the given code
```

The current oracle stops. The server continues.

### 13.35 HALT

Syntax source: `HALT;`

Semantics:

```text
terminate oracle successfully
```

### 13.36 NOP

Syntax source: `NOP;`

Semantics: nothing.

### 13.37 Bytecode targets

Jump and call targets are absolute instruction indexes inside the program. Labels are resolved by the compiler. Calls to oracle names are resolved during loading after all oracles in a file are known.

A call target that does not exist is a load error and the file is skipped.

### 13.38 Source positions

Each emitted instruction records the source line and column. They appear in disassembly and instruction traces. Compile errors include the file, line and column.

### 13.39 Disassembly format

The disassembler prints one line per instruction:

```text
<index> <OPCODE> <line>:<column> <operands>
```

Function entries are printed at the top:

```text
program scripts/example.obl
oracle @on_join = 0
0 MOV 4:9 $a0, #0
1 HOST 5:9 #2
2 HALT 6:9
```

The exact formatting is intended for debugging and may change.

## 14. Host calls

Host calls are the only way for OBLVN to affect the server. They are invoked with `SUMMON(#id)`.

### 14.1 Calling convention

- Arguments are read from `$a0`, `$a1`, `$a2` and `$a3`.
- The result, if any, is written to `$r0`.
- Other registers are preserved.
- Flags are preserved by host calls.
- Host calls do not trap unless the host id is unknown or a string id cannot be resolved.
- Host calls may produce server side effects immediately.

### 14.2 Host call 1: broadcast integer

```obl
BIND($a0) ::= #123;
SUMMON(#1);
```

Broadcasts the decimal representation of `$a0` to all players as a system chat message.

Side effects:

- sends a chat packet to every online player;
- writes a server log line through the normal broadcast path.

Result: none.

Errors: none.

### 14.3 Host call 2: broadcast string

```obl
BIND($a0) ::= SIGIL("&eHello");
SUMMON(#2);
```

Broadcasts a string id as a system chat message. The id is resolved against the current file's compile-time string table and the invocation's runtime string table.

Side effects:

- sends a chat packet to every online player;
- writes a server log line through the normal broadcast path.

Result: none.

Errors: an id that is neither a compile-time string nor a runtime string traps with `unknown string id`.

### 14.4 Host call 3: player count

```obl
SUMMON(#3);
BIND($a0) ::= $r0;
SUMMON(#1);
```

Writes the number of online players to `$r0`.

Result: online player count.

Errors: none.

### 14.5 Host call 4: current time

```obl
SUMMON(#4);
```

Writes the current Unix epoch time in milliseconds to `$r0`.

Result: integer milliseconds.

Errors: none.

### 14.6 Host call 5: get block

```obl
BIND($a0) ::= #10;
BIND($a1) ::= #64;
BIND($a2) ::= #10;
SUMMON(#5);
```

Reads a block state at the given coordinates into `$r0`.

Arguments:

- `$a0` x;
- `$a1` y;
- `$a2` z.

Result: 16-bit block state, zero-extended.

If `y` is outside `0..255`, the result is `0`.

### 14.7 Host call 6: set block

```obl
BIND($a0) ::= #10;
BIND($a1) ::= #64;
BIND($a2) ::= #10;
BIND($a3) ::= #1;
SUMMON(#6);
```

Sets a block state at the given coordinates.

Arguments:

- `$a0` x;
- `$a1` y;
- `$a2` z;
- `$a3` 16-bit block state.

If `y` is outside `0..255`, the call does nothing. The state is masked to 16 bits.

Side effects:

- updates the world;
- broadcasts a block change;
- schedules fluid updates;
- refreshes lighting when needed;
- starts falling blocks or grass growth when applicable.

Result: none.

### 14.8 Host call 7: log integer

```obl
BIND($a0) ::= $r0;
SUMMON(#7);
```

Writes the integer to the server log with a script prefix.

Result: none.

### 14.8.1 Host call 8: broadcast labeled integer

```obl
BIND($a0) ::= SIGIL("&ePlayers:");
BIND($a1) ::= $r0;
SUMMON(#8);
```

Broadcasts a string id followed immediately by the decimal representation of
`$a1`. This is equivalent to two broadcasts without the intermediate chat
packet.

Arguments:

- `$a0` string id;
- `$a1` integer value.

Result: none.

Errors: an id that cannot be resolved traps with `unknown string id`.

### 14.9 Unknown host calls

Any id other than 1 through 8 traps with `unknown host call <id>`.

### 14.10 Host call examples

Announce block state changes:

```obl
BIND($a0) ::= SIGIL("&eBlock state:");
SUMMON(#2);
BIND($a0) ::= $r0;
SUMMON(#1);
```

Place a stone block at a known position:

```obl
BIND($a0) ::= #10;
BIND($a1) ::= #65;
BIND($a2) ::= #10;
BIND($a3) ::= #16;
SUMMON(#6);
```

## 15. Events

### 15.1 Dispatch model

The server calls `fire(event, arguments, runtimeStrings)`:

1. A recursive script lock is acquired.
2. Every loaded file is checked for an oracle with the event name.
3. Every matching oracle runs in load order.
4. Runtime strings are installed for the invocation.
5. Arguments are copied into `$a0` through `$a3`.
6. The oracle runs until halt, return, trap or instruction limit.
7. The lock is released.

A trap in one file does not stop other files. A trap is logged and the current invocation ends.

### 15.2 on_join

Fires after a player has joined and the join message has been sent.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | runtime string id for the player name |
| `$a1` | entity id |

Example:

```obl
ORACLE @on_join ~> {
    BIND($a0) ::= SIGIL("&eWelcome, traveler");
    SUMMON(#2);
    HALT;
}
```

### 15.3 on_leave

Fires after a player has left and the leave message has been sent.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | runtime string id for the player name |

Example:

```obl
ORACLE @on_leave ~> {
    BIND($a0) ::= SIGIL("&7A player departed");
    SUMMON(#2);
    HALT;
}
```

### 15.4 on_chat

Fires after a chat message has been broadcast.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | runtime string id for the player name |
| `$a1` | runtime string id for the message |
| `$a2` | entity id |

Example:

```obl
ORACLE @on_chat ~> {
    BIND($a0) ::= SIGIL("&8[chat]&r ");
    SUMMON(#2);
    BIND($a0) ::= $a1;
    SUMMON(#2);
    HALT;
}
```

This example repeats chat with a prefix. Most servers do not want this behavior by default.

### 15.5 on_block_break

Fires after a block has been broken.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | x |
| `$a1` | y |
| `$a2` | z |
| `$a3` | old block state |

Example:

```obl
ORACLE @on_block_break ~> {
    BIND($r0) ::= $a3 </> #16;
    VERIFY($r0) <=> #50;
    WHEN::EQUAL => GATE @torch;
    GATE @done;
    @torch::
    BIND($a0) ::= SIGIL("&6A torch was broken");
    SUMMON(#2);
    @done::
    HALT;
}
```

### 15.6 on_block_place

Fires after a block has been placed.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | x |
| `$a1` | y |
| `$a2` | z |
| `$a3` | new block state |

Example:

```obl
ORACLE @on_block_place ~> {
    BIND($r0) ::= $a1;
    VERIFY($r0) <=> #64;
    WHEN::GREATER => GATE @high;
    GATE @done;
    @high::
    BIND($a0) ::= SIGIL("&eA block was placed high up");
    SUMMON(#2);
    @done::
    HALT;
}
```

### 15.7 on_death

Fires after a player death has been processed.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | runtime string id for the player name |
| `$a1` | entity id |

Example:

```obl
ORACLE @on_death ~> {
    BIND($a0) ::= SIGIL("&cA player has fallen");
    SUMMON(#2);
    HALT;
}
```

### 15.8 on_command

Fires before command dispatch, after operator permission checks.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | runtime string id for the command name |
| `$a1` | runtime string id for the argument text |
| `$a2` | entity id, or 0 for console |

Example:

```obl
ORACLE @on_command ~> {
    BIND($a0) ::= SIGIL("&8[command]&r ");
    SUMMON(#2);
    BIND($a0) ::= $a0;
    SUMMON(#2);
    HALT;
}
```

The second broadcast in this example reuses `$a0`, which was overwritten by the first string. To print both command and arguments, copy them to general registers first:

```obl
ORACLE @on_command ~> {
    BIND($r0) ::= $a0;
    BIND($r1) ::= $a1;
    BIND($a0) ::= SIGIL("&8[command]&r ");
    SUMMON(#2);
    BIND($a0) ::= $r0;
    SUMMON(#2);
    BIND($a0) ::= SIGIL("&8 ");
    SUMMON(#2);
    BIND($a0) ::= $r1;
    SUMMON(#2);
    HALT;
}
```

### 15.9 on_tick

Fires from the server tick thread at the configured script tick interval.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | monotonically increasing script tick counter |

The counter starts at 1 on the first script tick after startup and increases by one per script tick.

Example:

```obl
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
```

This broadcasts the player count once per minute when at least one player is online.

### 15.9.1 on_server_start

Fires once after script loading finishes and before the tick thread starts.

Arguments: none.

This is the place to initialize memory cells, magic values and version markers.

```obl
ORACLE @on_server_start ~> {
    BIND($r0) ::= #0;
    SCRIBE MEM[$r0 + #0] ::= #1;
    HALT;
}
```

### 15.9.2 on_server_stop

Fires once during shutdown, after the tick thread has stopped and before the
network layer is closed.

Arguments: none.

Use host call 7 to log final counters. Avoid broadcasts if players are already
disconnecting.

```obl
ORACLE @on_server_stop ~> {
    BIND($r0) ::= #0;
    REVEAL($a0) ::= MEM[$r0 + #0];
    SUMMON(#7);
    HALT;
}
```

### 15.9.3 on_player_damage

Fires after a player's health has been reduced and before death processing.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | entity id |
| `$a1` | damage amount, truncated to an integer |
| `$a2` | resulting health, truncated to an integer |

Example:

```obl
ORACLE @on_player_damage ~> {
    BIND($r0) ::= #0;
    REVEAL($r1) ::= MEM[$r0 + #0];
    BIND($r1) ::= $r1 <+> $a1;
    SCRIBE MEM[$r0 + #0] ::= $r1;
    HALT;
}
```

Damage amounts are floats inside the server. OBLVN receives integer
truncation, so `2.5` arrives as `2`.

### 15.9.4 on_respawn

Fires after a dead player has respawned.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | entity id |

Example:

```obl
ORACLE @on_respawn ~> {
    BIND($r0) ::= #0;
    REVEAL($r1) ::= MEM[$r0 + #0];
    INCREMENT($r1);
    SCRIBE MEM[$r0 + #0] ::= $r1;
    HALT;
}
```

### 15.9.5 on_item_pickup

Fires after a player picks up item entities.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | player entity id |
| `$a1` | item id |
| `$a2` | picked up count |
| `$a3` | item damage |

For a partial pickup, `$a2` is the amount picked up. For a full pickup, it is
the original stack count.

Example:

```obl
ORACLE @on_item_pickup ~> {
    BIND($r0) ::= #0;
    REVEAL($r1) ::= MEM[$r0 + #0];
    BIND($r1) ::= $r1 <+> $a2;
    SCRIBE MEM[$r0 + #0] ::= $r1;
    HALT;
}
```

### 15.9.6 on_item_drop

Fires after a player drops an item entity.

Arguments:

| Register | Value |
| --- | --- |
| `$a0` | player entity id |
| `$a1` | item id |
| `$a2` | dropped count |
| `$a3` | item damage |

Example:

```obl
ORACLE @on_item_drop ~> {
    BIND($a0) ::= SIGIL("&7Item dropped");
    SUMMON(#2);
    HALT;
}
```

### 15.10 Multiple files

If two files define `on_join`, both run. Each file has its own memory and string table. The order is the sorted file order used during loading.

### 15.11 Event cancellation

There is no cancellation. Event oracles observe what has already happened. They can perform additional side effects, but they cannot prevent the original action.

### 15.12 Event failures

A runtime trap in one event oracle does not crash the server and does not stop other files from handling the same event. The trap is logged.

### 15.13 Manual event dispatch

`/script run <oracle>` invokes an oracle without event arguments. This is useful for testing and for oracles that do not correspond to an event name. Runtime strings are not available, so host call 2 must use compile-time `SIGIL` ids.

### 15.14 Event ordering summary

| Event | Server action already done |
| --- | --- |
| `on_join` | yes, login finished and join message sent |
| `on_leave` | yes, player removed and leave message sent |
| `on_chat` | yes, chat broadcast |
| `on_block_break` | yes, block removed and drops scheduled |
| `on_block_place` | yes, block placed |
| `on_death` | yes, inventory dropped and death state set |
| `on_command` | no, fires before command dispatch |
| `on_tick` | not applicable |
| `on_server_start` | yes, scripts loaded |
| `on_server_stop` | yes, tick thread stopped, network still open |
| `on_player_damage` | health reduced, death not processed yet |
| `on_respawn` | respawn sequence complete |
| `on_item_pickup` | item collection applied |
| `on_item_drop` | item entity spawned |

## 16. Error catalog

This section lists the errors produced by the lexer, parser, loader and virtual machine, with causes and fixes.

### 16.1 Compile error format

Compile errors are reported as:

```text
<file>:<line>:<column>: <message>
```

Example:

```text
scripts/broken.obl:12:9: expected ;, got identifier HALT
```

The file is skipped. Other files continue to load.

### 16.2 Lexical errors

#### unexpected character

Example:

```text
scripts/broken.obl:3:7: unexpected character '@'
```

Cause: an unsupported character appears outside a token.

Fix: use a valid symbol, register, label, number or string.

#### expected digits after #

Example:

```text
scripts/broken.obl:4:12: expected digits after #
```

Cause: `#` is not followed by digits or `#-` followed by digits.

Fix: write a valid integer such as `#0` or `#-7`.

#### number is too large

Cause: the integer does not fit in signed 64 bits.

Fix: use a value in the range `-9223372036854775808..9223372036854775807`. Note that the lexer reads an optional minus sign after `#`, so `#-9223372036854775808` is the minimum value.

#### unterminated string

Cause: a string literal reaches the end of a line or the end of the file without a closing quote.

Fix: add the closing quote. Strings cannot span lines; use `\n` instead.

#### unknown escape

Cause: a backslash is followed by an unsupported escape character.

Fix: use `\\`, `\"`, `\n`, `\t` or `\r`.

#### expected register name

Cause: `$` is not followed by a valid identifier.

Fix: use `$r0` through `$r11` or `$a0` through `$a3`.

#### expected label name

Cause: `@` is not followed by a valid identifier.

Fix: use `@name`.

### 16.3 Parse errors

#### expected ARCANA

Cause: the top level contains something other than `ARCANA`.

Fix: every file-level group must start with `ARCANA "name" { ... }`.

#### expected arcana name

Cause: `ARCANA` is not followed by a string.

Fix: write `ARCANA "module" { ... }`.

#### expected {

Cause: a block opener is missing.

Fix: add `{`.

#### expected }

Cause: a block is not closed before the end of the file.

Fix: add `}`.

#### expected ORACLE

Cause: an arcana block contains a statement that is not an oracle.

Fix: only `ORACLE` declarations are allowed inside `ARCANA`.

#### expected oracle name

Cause: `ORACLE` is not followed by a label.

Fix: write `ORACLE @name ~> { ... }`.

#### duplicate oracle

Cause: two oracles in one file have the same name.

Fix: rename one oracle. Oracle names must be unique per file.

#### expected ~>

Cause: the oracle header is missing `~>`.

Fix: write `ORACLE @name ~> { ... }`.

#### duplicate label

Cause: two labels in one oracle have the same name.

Fix: rename one label.

#### unknown label

Cause: a `GATE`, `WHEN ... GATE` or local `CALL` refers to a label that is not defined in the oracle.

Fix: define the label or correct its spelling. Labels are case-sensitive.

#### unknown condition

Cause: `WHEN::` is followed by an unsupported condition name.

Fix: use one of the conditions in the condition table.

#### unknown statement

Cause: an identifier at statement position is not a known statement keyword.

Fix: check spelling and keyword case. Keywords are uppercase.

#### expected operand

Cause: a value is required but the token is not a register, number or `SIGIL("...")`.

Fix: supply a register, integer or string id.

#### expected memory operand

Cause: `REVEAL`, `REVEAL8`, `SCRIBE` or `SCRIBE8` is missing `MEM` or `MEM8`.

Fix: write the correct memory keyword.

#### expected offset or index register

Cause: `MEM[...]` has a `+` followed by something other than a number or register.

Fix: use `+ #offset` or `+ $register`.

#### expected ;

Cause: a statement is not terminated.

Fix: add `;`. OBLVN has no automatic semicolon insertion.

### 16.4 Load errors

#### failed to read script

Cause: the file exists but cannot be opened.

Fix: check permissions and path.

#### unknown oracle

Cause: a `CALL @name` refers to an oracle that is not defined in the same file and is not a local label.

Fix: define the oracle in the same file. Cross-file calls are not supported.

#### script directory not found

Cause: `script-dir` points to a missing directory.

Fix: create the directory or correct the setting.

#### script path is not a directory

Cause: `script-dir` points to a regular file.

Fix: point it at a directory.

### 16.5 Runtime traps

Runtime traps use this format:

```text
<file>: <oracle> pc <n>: <message>
```

Example:

```text
scripts/stats.obl: on_tick pc 17: division by zero
```

A trap stops the current oracle. The server continues.

#### unknown oracle

Cause: manual dispatch names an oracle that does not exist in a file, or an internal call target is invalid.

Fix: check the oracle name and the file.

#### too many arguments

Cause: more than four event arguments plus runtime strings are passed.

Fix: this is an internal API limit. Event dispatchers must pass at most four values.

#### execution left code

Cause: execution reached the end of the instruction stream without `HALT` or a top-level `RETURN`.

Fix: terminate every path. Add `HALT;` or `RETURN;`.

#### instruction limit reached

Cause: the oracle executed more instructions than the configured limit.

Fix: reduce loops, add an exit condition, or raise `script-max-instructions` if the workload is legitimate. A permanent loop should not be fixed by raising the limit.

#### memory read out of range

Cause: `REVEAL` or `REVEAL8` computed an address outside `0..4095`.

Fix: validate the base and index, or keep addresses in range.

#### memory write out of range

Cause: `SCRIBE` or `SCRIBE8` computed an address outside `0..4095`.

Fix: validate the base and index, or keep addresses in range.

#### division by zero

Cause: `</>` used a zero divisor.

Fix: compare the divisor with zero first.

#### modulo by zero

Cause: `<%>` used a zero divisor.

Fix: compare the divisor with zero first.

#### stack overflow

Cause: `PUSH` exceeded 256 data stack entries.

Fix: balance pushes and pops, or redesign the algorithm.

#### stack underflow

Cause: `POP` ran with an empty data stack.

Fix: check your push/pop balance.

#### call stack overflow

Cause: more than 64 nested calls.

Fix: reduce recursion or convert the algorithm to loops and memory.

#### unknown string id

Cause: host call 2 or host call 8 received an id that is neither in the compile-time string table nor in the runtime string table.

Fix: use `SIGIL("...")` or a runtime string id supplied by an event.

#### unknown host call

Cause: `SUMMON(#id)` used an unsupported id.

Fix: use ids 1 through 8.

#### trap

Cause: `TRAP(#code)` executed.

Fix: inspect the code and the branch that reached it.

### 16.6 Error recovery

- A compile error skips one file.
- A load error skips one file.
- A runtime trap stops one oracle invocation.
- A trap does not stop the server.
- A trap does not stop other files handling the same event.
- Reload compiles all files again.

### 16.7 Common mistakes

#### Using `EQUAL` after arithmetic

Wrong:

```obl
BIND($r0) ::= $a0 <-> #1;
WHEN::EQUAL => GATE @one;
```

Right:

```obl
BIND($r0) ::= $a0 <-> #1;
WHEN::ZERO => GATE @one;
```

#### Forgetting HALT

Wrong:

```obl
ORACLE @on_join ~> {
    BIND($a0) ::= SIGIL("hello");
    SUMMON(#2);
}
```

Right:

```obl
ORACLE @on_join ~> {
    BIND($a0) ::= SIGIL("hello");
    SUMMON(#2);
    HALT;
}
```

Without a terminator, execution leaves the code and traps.

#### Using a runtime string id in a manual dispatch

`/script run <oracle>` supplies no runtime strings. Host call 2 must use a compile-time `SIGIL` id.

#### Assuming memory is reset per invocation

Memory persists per file. If a counter keeps growing across events, that is expected. Use `/script reload` to reset it.

#### Assuming registers persist

Registers are reset to zero at each invocation. Use memory for persistent values.

#### Using `$r12`

`$r12` is not valid syntax. Use `$a0` through `$a3` for registers 12 through 15.

## 17. Debugging and disassembly

### 17.1 Debug logging

Script diagnostics use the normal server logger. Enable debug output:

```properties
debug=true
```

Without `debug=true`, trace and disassembly messages are suppressed even if `script-trace` or `script-disassemble` is enabled.

### 17.2 Trace

```properties
script-trace=true
```

The VM logs one line per executed instruction:

```text
script trace: on_join pc 0 MOV 4:9
script trace: on_join pc 1 HOST 5:9
script trace: on_join pc 2 HALT 6:9
```

Trace is useful for short oracles. It produces a large amount of output for loops.

### 17.3 Disassembly

```properties
script-disassemble=true
```

Each file's compiled program is logged at load:

```text
script disassembly:
program scripts/welcome.obl
oracle @on_join = 0
oracle @on_block_break = 3
0 MOV 4:9 $a0, #0
1 HOST 5:9 #2
2 HALT 6:9
3 SUB 10:9 $r0, #16
4 CMP 11:9 $r0, #50
5 JE 12:9 9
6 JMP 13:9 12
7 NOP 14:9
8 MOV 15:9 $a0, #1
9 HOST 16:9 #2
10 HALT 17:9
```

The exact operand formatting may change. The instruction index is stable for one compilation.

### 17.4 Load count

The server logs:

```text
loaded 3 script(s), 9 oracle(s)
```

If the count is lower than expected, at least one file failed to load. Check the preceding error lines.

### 17.5 Reload workflow

1. Edit a script.
2. Run `/script reload`.
3. Read the load count and any compile errors.
4. Run `/script run <oracle>` to test a specific oracle.
5. Enable trace for a short test if needed.
6. Disable trace and disassembly in production.

### 17.6 Test oracles

Name test oracles distinctly and do not use event names:

```obl
ORACLE @test_counter ~> {
    BIND($r0) ::= #0;
    REVEAL($r1) ::= MEM[$r0 + #0];
    INCREMENT($r1);
    SCRIBE MEM[$r0 + #0] ::= $r1;
    BIND($a0) ::= $r1;
    SUMMON(#1);
    HALT;
}
```

Run it with:

```text
/script run test_counter
```

Event oracles run automatically. Test oracles run only when requested.

### 17.7 Inspecting memory

There is no memory dump command. Use a test oracle to broadcast cells:

```obl
ORACLE @dump_memory ~> {
    BIND($r0) ::= #0;
    @loop::
    REVEAL($r1) ::= MEM[$r0 + #0];
    BIND($a0) ::= $r1;
    SUMMON(#1);
    INCREMENT($r0);
    VERIFY($r0) <=> #16;
    WHEN::LESS => GATE @loop;
    HALT;
}
```

This prints the first 16 cells.

### 17.8 Debugging recommendations

- Start with one small oracle.
- Use `/script run` before enabling an event.
- Keep event oracles short.
- Prefer memory writes over chat spam for debugging in production.
- Use host call 7 to log an integer without broadcasting.
- Remove trace output after debugging.

## 18. Configuration reference

### 18.1 Script settings

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `scripts-enabled` | `true` | boolean | load and run scripts |
| `script-dir` | `scripts` | path | directory containing `.obl` files |
| `script-tick-interval-ms` | `1000` | `100..3600000` | interval between `on_tick` events |
| `script-max-instructions` | `200000` | `1000..10000000` | per-invocation instruction limit |
| `script-trace` | `false` | boolean | log every executed instruction at debug level |
| `script-disassemble` | `false` | boolean | log compiled bytecode at load |

### 18.2 Related server settings

| Key | Default | Meaning |
| --- | --- | --- |
| `debug` | `false` | enables debug-level logging, required for trace and disassembly output |
| `log-file` | `server.log` | server log destination |

### 18.3 Example configuration

```properties
scripts-enabled=true
script-dir=scripts
script-tick-interval-ms=1000
script-max-instructions=200000
script-trace=false
script-disassemble=false
```

### 18.4 Development configuration

```properties
debug=true
scripts-enabled=true
script-dir=scripts
script-tick-interval-ms=1000
script-max-instructions=200000
script-trace=true
script-disassemble=true
```

### 18.5 Production configuration

```properties
debug=false
scripts-enabled=true
script-dir=scripts
script-tick-interval-ms=1000
script-max-instructions=200000
script-trace=false
script-disassemble=false
```

### 18.6 Disabling scripts

```properties
scripts-enabled=false
```

No files are loaded and no events are dispatched. `/script reload` still clears the engine, but no scripts load.

### 18.7 Changing the limit

The instruction limit is a safety valve, not a performance target. A legitimate script should normally execute far fewer than 200000 instructions. If a script needs more, first check for an accidental infinite loop.

## 19. Tutorial

This tutorial builds OBLVN programs from empty files to event handlers.

### 19.1 Create a file

Create `scripts/tutorial.obl`:

```obl
ARCANA "tutorial" {
    ORACLE @hello ~> {
        BIND($a0) ::= SIGIL("&eHello from OBLVN");
        SUMMON(#2);
        HALT;
    }
}
```

### 19.2 Load and run

Start the server. The log should contain:

```text
loaded 1 script(s), 1 oracle(s)
```

Run the oracle manually:

```text
/script run hello
```

The chat receives the greeting. The oracle is not named after an event, so it does not run automatically.

### 19.3 Add arithmetic

```obl
ARCANA "tutorial" {
    ORACLE @math ~> {
        BIND($r0) ::= #10;
        BIND($r1) ::= #32;
        BIND($r2) ::= $r0 <+> $r1;
        BIND($a0) ::= $r2;
        SUMMON(#1);
        HALT;
    }
}
```

`/script run math` broadcasts `42`.

### 19.4 Add memory

```obl
ARCANA "tutorial" {
    ORACLE @memory ~> {
        BIND($r0) ::= #0;
        SCRIBE MEM[$r0 + #0] ::= #100;
        REVEAL($r1) ::= MEM[$r0 + #0];
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }
}
```

`/script run memory` broadcasts `100`.

### 19.5 Make memory persistent

```obl
ARCANA "tutorial" {
    ORACLE @counter ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }
}
```

Run `counter` several times. It broadcasts 1, 2, 3 and so on. Reload resets it.

### 19.6 Add a condition

```obl
ARCANA "tutorial" {
    ORACLE @threshold ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        VERIFY($r1) <=> #3;
        WHEN::LESS => GATE @low;
        BIND($a0) ::= SIGIL("&cThreshold reached");
        SUMMON(#2);
        HALT;
        @low::
        BIND($a0) ::= SIGIL("&aBelow threshold");
        SUMMON(#2);
        HALT;
    }
}
```

### 19.7 Add a call

```obl
ARCANA "tutorial" {
    ORACLE @calls ~> {
        BIND($r0) ::= #41;
        CALL @increment;
        BIND($a0) ::= $r0;
        SUMMON(#1);
        HALT;
        @increment::
        INCREMENT($r0);
        RETURN;
    }
}
```

`/script run calls` broadcasts `42`.

### 19.8 Add an event

Rename the oracle to `on_join`:

```obl
ARCANA "tutorial" {
    ORACLE @on_join ~> {
        BIND($a0) ::= SIGIL("&eWelcome to the tutorial");
        SUMMON(#2);
        HALT;
    }
}
```

Reload and join. The message appears automatically.

### 19.9 Use event arguments

```obl
ARCANA "tutorial" {
    ORACLE @on_block_break ~> {
        BIND($r0) ::= $a3 </> #16;
        VERIFY($r0) <=> #50;
        WHEN::EQUAL => GATE @torch;
        GATE @done;
        @torch::
        BIND($a0) ::= SIGIL("&6Torch broken");
        SUMMON(#2);
        @done::
        HALT;
    }
}
```

Break a torch. The oracle fires.

### 19.10 Use a byte cell

```obl
ARCANA "tutorial" {
    ORACLE @bytes ~> {
        BIND($r0) ::= #0;
        SCRIBE8 MEM8[$r0 + #0] ::= #300;
        REVEAL8($r1) ::= MEM8[$r0 + #0];
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }
}
```

`/script run bytes` broadcasts `44`.

### 19.11 Use an index register

```obl
ARCANA "tutorial" {
    ORACLE @array ~> {
        BIND($r0) ::= #0;
        BIND($r1) ::= #1;
        BIND($r2) ::= #2;
        SCRIBE MEM[$r0 + #0] ::= #10;
        SCRIBE MEM[$r0 + #8] ::= #20;
        SCRIBE MEM[$r0 + #16] ::= #30;
        REVEAL($r3) ::= MEM[$r0 + $r2];
        BIND($a0) ::= $r3;
        SUMMON(#1);
        HALT;
    }
}
```

With `$r2 = 2` this broadcasts `0`, because memory addresses are cell indexes, not byte offsets. The example writes cells 0, 8 and 16. Use `BIND($r2) ::= #16;` to read the third written word and broadcast `30`.

### 19.12 Use a byte array

```obl
ARCANA "tutorial" {
    ORACLE @byte_array ~> {
        BIND($r0) ::= #0;
        SCRIBE8 MEM8[$r0 + #0] ::= #10;
        SCRIBE8 MEM8[$r0 + #1] ::= #20;
        BIND($r1) ::= #1;
        REVEAL8($r2) ::= MEM8[$r0 + $r1];
        BIND($a0) ::= $r2;
        SUMMON(#1);
        HALT;
    }
}
```

### 19.13 Debug the tutorial

If nothing happens:

1. Check the load count in the log.
2. Run `/script reload`.
3. Read compile errors.
4. Run `/script run <oracle>`.
5. Enable `debug=true` and `script-trace=true`.
6. Check that the oracle is named after the intended event.
7. Check that players are online for broadcasts.

### 19.14 Next steps

Read the recipes in the next section, then use the opcode reference when disassembly is needed.

## 20. Recipes

### 20.1 Welcome message with join counter

```obl
ARCANA "joins" {
    ORACLE @on_join ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= SIGIL("&eWelcome to the server");
        SUMMON(#2);
        BIND($a0) ::= SIGIL("&7Joins so far:");
        SUMMON(#2);
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }
}
```

Memory cell 0 is the join counter. It persists until reload.

### 20.2 Log block break coordinates

```obl
ARCANA "audit" {
    ORACLE @on_block_break ~> {
        BIND($a0) ::= $a0;
        SUMMON(#7);
        BIND($a0) ::= $a1;
        SUMMON(#7);
        BIND($a0) ::= $a2;
        SUMMON(#7);
        BIND($a0) ::= $a3;
        SUMMON(#7);
        HALT;
    }
}
```

Host call 7 logs without broadcasting. The first line logs x, then y, then z, then block state. If the coordinates appear in the wrong order, check that host call 7 does not modify `$a0`; it does not, but the script overwrites `$a0` deliberately on each line.

### 20.3 Announce high block placement

```obl
ARCANA "build" {
    ORACLE @on_block_place ~> {
        BIND($r0) ::= $a1;
        VERIFY($r0) <=> #100;
        WHEN::LESS => GATE @done;
        BIND($a0) ::= SIGIL("&eA block was placed above y=100");
        SUMMON(#2);
        @done::
        HALT;
    }
}
```

### 20.4 Periodic player count

```obl
ARCANA "stats" {
    ORACLE @on_tick ~> {
        BIND($r0) ::= $a0 <%> #60;
        VERIFY($r0) <=> #0;
        WHEN::NOT_EQUAL => GATE @quiet;
        SUMMON(#3);
        VERIFY($r0) <=> #0;
        WHEN::EQUAL => GATE @quiet;
        BIND($a0) ::= SIGIL("&aOnline players:");
        SUMMON(#2);
        BIND($a0) ::= $r0;
        SUMMON(#1);
        @quiet::
        HALT;
    }
}
```

With the default tick interval this runs once per minute. The first `VERIFY` compares the tick modulo 60 with zero. The second checks that at least one player is online.

### 20.5 Command watcher

```obl
ARCANA "commands" {
    ORACLE @on_command ~> {
        BIND($r0) ::= $a0;
        BIND($r1) ::= $a1;
        BIND($a0) ::= SIGIL("&8[cmd]&r ");
        SUMMON(#2);
        BIND($a0) ::= $r0;
        SUMMON(#2);
        BIND($a0) ::= SIGIL("&8 ");
        SUMMON(#2);
        BIND($a0) ::= $r1;
        SUMMON(#2);
        HALT;
    }
}
```

This announces every command to all players. In production this is often too noisy. Use host call 7 to log commands instead of broadcasting them.

### 20.6 Memory array with an index register

```obl
ARCANA "array" {
    ORACLE @on_block_break ~> {
        BIND($r0) ::= #0;
        BIND($r1) ::= $a3 <&> #15;
        SCRIBE MEM[$r0 + $r1] ::= $a3;
        BIND($a0) ::= $a3;
        SUMMON(#1);
        HALT;
    }
}
```

This stores the low four bits as an index. Only 16 cells are used. Validate the index before using it in production if the input is not masked.

A safer form:

```obl
BIND($r1) ::= $a3 <&> #15;
VERIFY($r1) <=> #16;
WHEN::GREATER_OR_EQUAL => GATE @invalid;
```

### 20.7 Safe division

```obl
ARCANA "safe_div" {
    ORACLE @divide ~> {
        BIND($r1) ::= $a1;
        VERIFY($r1) <=> #0;
        WHEN::EQUAL => GATE @zero;
        BIND($r0) ::= $a0 </> $r1;
        BIND($a0) ::= $r0;
        SUMMON(#1);
        HALT;
        @zero::
        BIND($a0) ::= SIGIL("&cDivision by zero");
        SUMMON(#2);
        HALT;
    }
}
```

Run it with `/script run divide`. Manual dispatch supplies zero arguments, so it reports division by zero. To test a real division, use an event or a dedicated test oracle with constants.

### 20.8 Three-state machine

Memory cell 0 stores the state: 0, 1 or 2.

```obl
ARCANA "state" {
    ORACLE @on_join ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        VERIFY($r1) <=> #3;
        WHEN::LESS => GATE @store;
        BIND($r1) ::= #0;
        @store::
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }
}
```

The state cycles 0, 1, 2, 0, 1, 2.

### 20.9 Helper functions with a data stack

```obl
ARCANA "stack" {
    ORACLE @sum2 ~> {
        BIND($r0) ::= #17;
        BIND($r1) ::= #25;
        PUSH($r0);
        PUSH($r1);
        CALL @add_top;
        BIND($a0) ::= $r0;
        SUMMON(#1);
        HALT;
        @add_top::
        POP($r1);
        POP($r2);
        BIND($r0) ::= $r1 <+> $r2;
        RETURN;
    }
}
```

`/script run sum2` broadcasts 42.

### 20.10 Byte flags

```obl
ARCANA "flags" {
    ORACLE @on_join ~> {
        BIND($r0) ::= #0;
        REVEAL8($r1) ::= MEM8[$r0 + #0];
        BIND($r2) ::= #1;
        BIND($r3) ::= $r1 <&> $r2;
        VERIFY($r3) <=> #0;
        WHEN::EQUAL => GATE @seen;
        BIND($a0) ::= SIGIL("&eWelcome back");
        SUMMON(#2);
        GATE @done;
        @seen::
        BIND($a0) ::= SIGIL("&eFirst visit");
        SUMMON(#2);
        BIND($r1) ::= $r1 <|> #1;
        SCRIBE8 MEM8[$r0 + #0] ::= $r1;
        @done::
        HALT;
    }
}
```

The low bit of byte cell 0 records whether the player has been seen. This is a file-wide flag, not per-player state. Per-player state would need a stable player id mapped into memory, which OBLVN does not provide automatically.

### 20.11 World edit and announcement

```obl
ARCANA "world" {
    ORACLE @marker ~> {
        BIND($a0) ::= #0;
        BIND($a1) ::= #64;
        BIND($a2) ::= #0;
        BIND($a3) ::= #16;
        SUMMON(#6);
        BIND($a0) ::= SIGIL("&eStone marker placed at 0,64,0");
        SUMMON(#2);
        HALT;
    }
}
```

Block state 16 is stone with metadata 0. Use host call 5 to read an existing block state when the exact state is not known.

### 20.12 Copy a block

```obl
ARCANA "copy" {
    ORACLE @copy ~> {
        BIND($a0) ::= #10;
        BIND($a1) ::= #64;
        BIND($a2) ::= #10;
        SUMMON(#5);
        BIND($r0) ::= $r0;
        BIND($a0) ::= #11;
        BIND($a1) ::= #64;
        BIND($a2) ::= #10;
        BIND($a3) ::= $r0;
        SUMMON(#6);
        HALT;
    }
}
```

### 20.13 Rate limit with the tick counter

```obl
ARCANA "limit" {
    ORACLE @throttle ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        BIND($r2) ::= $a0 <-> $r1;
        VERIFY($r2) <=> #5;
        WHEN::LESS => GATE @skip;
        SCRIBE MEM[$r0 + #0] ::= $a0;
        BIND($a0) ::= SIGIL("&eAllowed");
        SUMMON(#2);
        @skip::
        HALT;
    }
}
```

`$a0` must be a monotonic tick counter for this recipe. `on_tick` supplies one. `on_join` supplies a name string id, so do not reuse this pattern without adapting the argument source.

### 20.14 Test oracle for memory

```obl
ARCANA "memory_tools" {
    ORACLE @test_memory ~> {
        BIND($r0) ::= #0;
        @loop::
        REVEAL($r1) ::= MEM[$r0 + #0];
        BIND($a0) ::= $r1;
        SUMMON(#1);
        INCREMENT($r0);
        VERIFY($r0) <=> #8;
        WHEN::LESS => GATE @loop;
        HALT;
    }
}
```

Run with `/script run test_memory` to print the first eight cells.

## 21. Patterns and performance

### 21.1 Memory layout

Choose a layout and document it in comments outside the script. A simple layout:

| Cells | Use |
| --- | --- |
| 0 | join counter |
| 1 | last tick |
| 2 | state flags |
| 4..7 | scratch |
| 16..31 | small array |
| 256..511 | larger table |

Because memory is shared by all oracles in a file, avoid overlapping regions by accident.

### 21.2 Counters

Counters use `REVEAL`, arithmetic and `SCRIBE`:

```obl
BIND($r0) ::= #0;
REVEAL($r1) ::= MEM[$r0 + #0];
INCREMENT($r1);
SCRIBE MEM[$r0 + #0] ::= $r1;
```

Counters are per-file, not per-player.

### 21.3 State machines

Use one cell per state machine and a bounded set of values. Validate transitions with `VERIFY` before writing.

### 21.4 Arrays

Use an index register to avoid long switch chains:

```obl
BIND($r0) ::= #16;
BIND($r1) ::= $a0 <&> #15;
SCRIBE MEM[$r0 + $r1] ::= $a1;
```

Always mask or validate the index. OBLVN does not bounds check array semantics, only the final memory address.

### 21.5 Stack discipline

Every `PUSH` needs a matching `POP` on every path. The stack is not automatically unwound when an oracle exits. Since it resets per invocation, leaks do not cross invocations, but they can overflow within one call.

### 21.6 Calls versus jumps

Use `CALL` for reusable logic that returns. Use `GATE` for local control flow that does not return. `HALT` in a called oracle exits everything, so helpers should normally end with `RETURN`.

### 21.7 Host call costs

Host call 2 and host call 1 broadcast to every player. Avoid calling them in tight loops. Host call 5 and host call 6 touch the world and may trigger lighting updates, fluid updates or falling blocks.

### 21.8 Tick budget

`on_tick` runs at a fixed interval. A heavy tick script delays subsequent ticks. Keep tick scripts short. If a task needs many world operations, split it across ticks with persistent memory:

```obl
BIND($r0) ::= #0;
REVEAL($r1) ::= MEM[$r0 + #0];
INCREMENT($r1);
SCRIBE MEM[$r0 + #0] ::= $r1;
```

### 21.9 Broadcast batching

Prefer one broadcast with a longer string over many broadcasts. OBLVN cannot concatenate strings, so use one message per logical line and avoid per-element chat spam.

### 21.10 String ids

Compile-time string ids are free to create but consume a small string table entry. Reusing one `SIGIL` is not deduplicated, so repeated `SIGIL("same")` adds multiple entries. This is harmless but wasteful.

### 21.11 Instruction estimates

Approximate instruction costs:

| Pattern | Instructions |
| --- | --- |
| one bind | 1 or 2 |
| one memory access | 1 |
| one compare and gate | 2 or 3 |
| one host call | 1 |
| a 10-iteration loop | roughly 50 to 100 |

These are rough. Use trace or disassembly when exact counts matter.

### 21.12 Measuring

Enable `script-trace` for one oracle and count trace lines. The final `pc` value in an error or trace gives the instruction index.

### 21.13 Reload cost

Reload recompiles all files. It is fast for small scripts but should not be done in a tight loop. Reload also resets persistent memory.

### 21.14 Testing

Test oracles should not use event names. Dispatch them with `/script run`. Keep a separate test file or use oracles whose names start with `test_`.

### 21.15 Version control

Scripts are source code. Keep them in version control. Do not edit production scripts without a reload and a test plan.

## 22. Security model

### 22.1 Trusted configuration

OBLVN scripts are trusted configuration. They run inside the server process with access to the host calls. Do not load scripts from untrusted sources.

### 22.2 No filesystem or network

The language itself cannot open files, open sockets, spawn processes or read environment variables. All effects go through host calls.

### 22.3 Bounded memory

Memory is fixed at 4096 cells. Address checks trap on out-of-range access. A script cannot use memory outside its own file.

### 22.4 Bounded time

The instruction limit stops runaway loops. The data stack and call stack are bounded. A script can still waste the configured instruction budget on every tick, so choose the limit deliberately.

### 22.5 Host side effects

Host call 6 can modify the world. Host calls 1 and 2 can broadcast chat. Host call 7 writes the log. Treat script write access like operator access.

### 22.6 Reload access

`/script reload` and `/script run` are operator commands. Only operators and the console should have access.

### 22.7 File permissions

The script directory should be writable only by the server administrator. A user who can write scripts can execute host calls with server privileges.

### 22.8 Logging

Compile errors, traps and trace output are written to the server log. Do not put secrets in scripts; strings may appear in logs and in chat.

### 22.9 Denial of service

A script with a large instruction limit or expensive host calls can degrade the server. Keep limits conservative and review scripts before deployment.

## 23. Migration notes

### 23.1 OBLVN 1 syntax remains valid

The following constructs are unchanged:

- `ARCANA` and `ORACLE`;
- `BIND`;
- `REVEAL` and `SCRIBE`;
- `VERIFY`;
- `GATE` and `WHEN`;
- `CALL` and `RETURN`;
- `PUSH` and `POP`;
- `SUMMON`;
- `HALT` and `NOP`;
- `SIGIL`.

Existing scripts continue to compile.

### 23.2 Memory is now persistent

In OBLVN 1, memory was zeroed on every invocation. In OBLVN 2, memory persists per file. If a script depended on zeroed memory, initialize the cells it uses at the start of the oracle, or reload scripts before each run.

### 23.3 ZERO and NONZERO semantics

In OBLVN 1, `ZERO` and `NONZERO` were aliases for `EQUAL` and `NOT_EQUAL`. In OBLVN 2, they use the zero flag, which is updated by arithmetic.

Immediately after `VERIFY` the behavior is the same. After arithmetic it is different and usually more useful.

Migration:

- after `VERIFY`, no change is needed;
- after arithmetic, `ZERO` is now correct and `EQUAL` may be stale;
- if you specifically want comparison equality after arithmetic, execute a new `VERIFY`.

### 23.4 New instructions

OBLVN 2 adds:

- `REVEAL8` and `SCRIBE8`;
- `NEGATE`, `COMPLEMENT`, `INCREMENT`, `DECREMENT`;
- `SWAP`;
- `TRAP`;
- carry and overflow conditions;
- indirect addresses.

### 23.5 Stack separation

The data stack and call stack are now separate. Scripts that used `PUSH` to store return addresses manually were relying on undefined behavior in OBLVN 1. Use `CALL` and `RETURN` instead.

### 23.6 New configuration

OBLVN 2 adds:

- `script-max-instructions`;
- `script-trace`;
- `script-disassemble`.

Existing configuration files remain valid; missing keys use defaults.

### 23.7 Disassembler

The disassembler is new. Enable `script-disassemble=true` and `debug=true` to inspect compiled programs.

### 23.8 New bit operations and events

OBLVN 2 also adds `POPCOUNT`, `LEADING_ZEROS`, `TRAILING_ZEROS`, `BYTESWAP`, `<rol>` and `<ror>`, host call 8, and the events `on_server_start`, `on_server_stop`, `on_player_damage`, `on_respawn`, `on_item_pickup` and `on_item_drop`.

Existing scripts do not need changes for these additions.

## 24. FAQ

### 24.1 How do I write a comment?

You do not. OBLVN has no comments. Keep notes in a separate document or in the surrounding repository documentation.

### 24.2 How do I print text?

Use `SIGIL("text")` and host call 2:

```obl
BIND($a0) ::= SIGIL("&eHello");
SUMMON(#2);
```

### 24.3 How do I concatenate strings?

There is no string concatenation. Broadcast separate messages or precompute complete strings with `SIGIL`.

### 24.4 How do I print a number?

Use host call 1:

```obl
BIND($a0) ::= $r0;
SUMMON(#1);
```

### 24.5 How do I read chat text?

Use the `on_chat` event. `$a1` is the message string id. Pass it to host call 2 to broadcast the text.

### 24.6 How do I cancel an event?

You cannot. Event handlers observe events after the server has acted, except `on_command`, which runs before dispatch but still cannot cancel the command.

### 24.7 How do I call an oracle in another file?

You cannot. Calls are resolved within one file. Duplicate event names across files all run for events, but they cannot call each other directly.

### 24.8 Is memory persistent?

Yes, per file, until reload or server restart. Registers, stacks and flags are not persistent.

### 24.9 How do I reset memory?

Run `/script reload`. This clears all programs and memories and recompiles the script directory.

### 24.10 Why does my EQUAL check fail after arithmetic?

`EQUAL` uses comparison state from `VERIFY`. Use `ZERO` after arithmetic, or run `VERIFY` first.

### 24.11 Why did my script not load?

Check the server log for a compile error with file, line and column. The load count will be lower than expected.

### 24.12 Why does my broadcast do nothing?

If no players are online, a broadcast has no visible recipients. Host call 7 logs independently of players.

### 24.13 Why does my script stop?

Check for a trap: memory out of range, division by zero, stack overflow, unknown string id, unknown host call or explicit `TRAP`.

### 24.14 Why did my loop never finish?

The instruction limit stopped it. Check the loop exit condition and the comparison state.

### 24.15 Can I use floating point?

No. OBLVN is integer-only.

### 24.16 Can I use booleans?

Use 0 and 1. Compare with zero.

### 24.17 Can I store strings in memory?

No. Strings are ids in a string table, not memory values. You can store ids as integers, but you cannot inspect or modify string contents.

### 24.18 Can I read a player's name in on_tick?

`on_tick` has no player context. Use `on_join`, `on_chat`, `on_death`, `on_leave` or `on_command` for player-specific arguments.

### 24.19 Can I get a player by name?

No direct host call exists. Player-specific events pass a name string id and entity id.

### 24.20 How many scripts can I load?

There is no explicit file limit. Each file costs a program, a string table and a 4096-cell memory array. Practical limits depend on server memory and reload time.

### 24.21 How many oracles can one file have?

There is no explicit oracle limit. Oracle names must be unique in the file.

### 24.22 Can two files define the same oracle?

Yes. An event fires both. `/script run` dispatches both. They do not share memory.

### 24.23 Are host calls thread-safe?

Script execution is serialized by the script lock. Host calls execute in that context. They may call server methods that have their own synchronization.

### 24.24 What happens if a script traps?

The current invocation stops and the error is logged. Other files and the server continue.

### 24.25 What happens if reload fails?

Files that fail to compile are skipped. Successfully compiled files load. The previous programs are gone because reload clears first.

### 24.26 Can I use OBLVN for large algorithms?

Use it for small event logic. Large algorithms belong in C++ server code.

### 24.27 Is the bytecode format stable?

No. It is an internal in-memory representation. There is no supported portable bytecode file format.

### 24.28 Can I generate OBLVN source at runtime?

The server compiles files from the script directory. It does not compile arbitrary source strings on demand. You can write files and run `/script reload`.

## 25. Glossary

### ARCANA

A file-level block that groups oracles. Syntax: `ARCANA "name" { ... }`.

### Oracle

A function. Syntax: `ORACLE @name ~> { ... }`. Event oracles are called by the server.

### Bytecode

The internal instruction stream produced by the compiler and executed by the virtual machine.

### Compile-time string

A string added by `SIGIL`. It receives a small integer id in the file's string table.

### Runtime string

A string supplied by an event. It receives an id from 100000 upward for the duration of one invocation.

### Memory cell

One signed 64-bit slot in the 4096-cell memory array.

### Word access

A 64-bit read or write with `REVEAL` or `SCRIBE`.

### Byte access

An 8-bit read or write with `REVEAL8` or `SCRIBE8`.

### Data stack

The stack used by `PUSH` and `POP`. Limit 256 entries.

### Call stack

The stack used by `CALL` and `RETURN`. Limit 64 entries.

### Comparison state

The less/equal/greater result recorded by `VERIFY`. Used by `EQUAL`, `LESS`, `GREATER` and their variants.

### Flag

A boolean state bit: zero, negative, carry or overflow.

### Trap

A controlled runtime failure. It stops the current oracle and logs an error.

### Host call

A native server function invoked with `SUMMON(#id)`.

### Dispatch

The act of firing an event or manually running an oracle.

### Reload

Recompiling all scripts with `/script reload`. Clears memory.

### Trace

Per-instruction debug logging controlled by `script-trace`.

### Disassembly

A textual listing of compiled instructions controlled by `script-disassemble`.

### Program counter

The index of the current instruction, abbreviated `pc` in error messages.

## 26. Appendix

### 26.1 Register quick reference

| Register | Alias | Notes |
| --- | --- | --- |
| 0 | `$r0` | host result |
| 1 | `$r1` | general |
| 2 | `$r2` | general |
| 3 | `$r3` | general |
| 4 | `$r4` | general |
| 5 | `$r5` | general |
| 6 | `$r6` | general |
| 7 | `$r7` | general |
| 8 | `$r8` | general |
| 9 | `$r9` | general |
| 10 | `$r10` | general |
| 11 | `$r11` | general |
| 12 | `$a0` | argument 0 |
| 13 | `$a1` | argument 1 |
| 14 | `$a2` | argument 2 |
| 15 | `$a3` | argument 3 |

### 26.2 Condition quick reference

| Condition | Opcode | Uses |
| --- | --- | --- |
| `EQUAL` | `JE` | comparison state |
| `NOT_EQUAL` | `JNE` | comparison state |
| `LESS` | `JL` | comparison state |
| `GREATER` | `JG` | comparison state |
| `LESS_OR_EQUAL` | `JLE` | comparison state |
| `LESS_EQUAL` | `JLE` | alias |
| `GREATER_OR_EQUAL` | `JGE` | comparison state |
| `GREATER_EQUAL` | `JGE` | alias |
| `ZERO` | `JZ` | zero flag |
| `NONZERO` | `JNZ` | zero flag |
| `CARRY` | `JC` | carry flag |
| `NO_CARRY` | `JNC` | carry flag |
| `OVERFLOW` | `JO` | overflow flag |
| `NO_OVERFLOW` | `JNO` | overflow flag |

### 26.3 Host call quick reference

| Id | Arguments | Result | Effect |
| --- | --- | --- | --- |
| 1 | `$a0` value | none | broadcast integer |
| 2 | `$a0` string id | none | broadcast string |
| 3 | none | `$r0` | player count |
| 4 | none | `$r0` | epoch milliseconds |
| 5 | `$a0` x, `$a1` y, `$a2` z | `$r0` | get block state |
| 6 | `$a0` x, `$a1` y, `$a2` z, `$a3` state | none | set block |
| 7 | `$a0` value | none | log integer |
| 8 | `$a0` string id, `$a1` value | none | broadcast labeled integer |

### 26.4 Event quick reference

| Event | `$a0` | `$a1` | `$a2` | `$a3` |
| --- | --- | --- | --- | --- |
| `on_join` | name string | entity id | | |
| `on_leave` | name string | | | |
| `on_chat` | name string | message string | entity id | |
| `on_block_break` | x | y | z | old state |
| `on_block_place` | x | y | z | new state |
| `on_death` | name string | entity id | | |
| `on_command` | command string | argument string | entity id | |
| `on_tick` | tick counter | | | |
| `on_server_start` | | | | |
| `on_server_stop` | | | | |
| `on_player_damage` | entity id | damage | new health | |
| `on_respawn` | entity id | | | |
| `on_item_pickup` | player entity | item id | count | damage |
| `on_item_drop` | player entity | item id | count | damage |

### 26.5 Configuration quick reference

| Key | Default | Purpose |
| --- | --- | --- |
| `scripts-enabled` | `true` | enable scripting |
| `script-dir` | `scripts` | script directory |
| `script-tick-interval-ms` | `1000` | on_tick interval |
| `script-max-instructions` | `200000` | invocation limit |
| `script-trace` | `false` | instruction trace |
| `script-disassemble` | `false` | load-time disassembly |
| `debug` | `false` | enable debug log output |

### 26.6 Limit quick reference

| Resource | Limit |
| --- | --- |
| registers | 16 |
| memory cells | 4096 |
| data stack | 256 |
| call stack | 64 |
| default instructions | 200000 |
| maximum instructions | 10000000 |
| runtime string id base | 100000 |
| block coordinate y range | 0..255 |

### 26.7 Error quick reference

| Error | Typical cause |
| --- | --- |
| `expected ;` | missing semicolon |
| `expected }` | unclosed block |
| `duplicate oracle` | same oracle name twice in a file |
| `unknown label` | gate or call to an undefined local label |
| `unknown oracle` | call to an oracle not in the same file |
| `execution left code` | missing HALT or RETURN |
| `instruction limit reached` | runaway loop |
| `memory read out of range` | bad address |
| `memory write out of range` | bad address |
| `division by zero` | zero divisor |
| `modulo by zero` | zero divisor |
| `stack overflow` | too many pushes |
| `stack underflow` | pop from empty data stack |
| `call stack overflow` | too much recursion |
| `unknown string id` | host call 2 with a bad id |
| `unknown host call` | host id outside 1..8 |
| `trap` | explicit TRAP |

### 26.8 Complete example: counter and announcer

```obl
ARCANA "announcer" {
    ORACLE @on_join ~> {
        BIND($r0) ::= #0;
        REVEAL($r1) ::= MEM[$r0 + #0];
        INCREMENT($r1);
        SCRIBE MEM[$r0 + #0] ::= $r1;
        BIND($a0) ::= SIGIL("&eWelcome");
        SUMMON(#2);
        BIND($a0) ::= SIGIL("&7Joins:");
        SUMMON(#2);
        BIND($a0) ::= $r1;
        SUMMON(#1);
        HALT;
    }

    ORACLE @on_tick ~> {
        BIND($r0) ::= $a0 <%> #300;
        VERIFY($r0) <=> #0;
        WHEN::NOT_EQUAL => GATE @quiet;
        SUMMON(#3);
        VERIFY($r0) <=> #0;
        WHEN::EQUAL => GATE @quiet;
        BIND($a0) ::= SIGIL("&aPlayers online:");
        SUMMON(#2);
        BIND($a0) ::= $r0;
        SUMMON(#1);
        @quiet::
        HALT;
    }
}
```

### 26.9 Complete example: block audit

```obl
ARCANA "audit" {
    ORACLE @on_block_break ~> {
        BIND($r0) ::= $a3 </> #16;
        VERIFY($r0) <=> #50;
        WHEN::EQUAL => GATE @torch;
        GATE @done;
        @torch::
        BIND($a0) ::= SIGIL("&6A torch was removed");
        SUMMON(#2);
        @done::
        HALT;
    }

    ORACLE @on_block_place ~> {
        BIND($r0) ::= $a3 </> #16;
        VERIFY($r0) <=> #50;
        WHEN::EQUAL => GATE @torch;
        GATE @done;
        @torch::
        BIND($a0) ::= SIGIL("&6A torch was placed");
        SUMMON(#2);
        @done::
        HALT;
    }
}
```

### 26.10 Complete example: arithmetic showcase

```obl
ARCANA "numbers" {
    ORACLE @test_numbers ~> {
        BIND($r0) ::= #100;
        BIND($r1) ::= #7;
        BIND($r2) ::= $r0 <+> $r1;
        BIND($r3) ::= $r0 <-> $r1;
        BIND($r4) ::= $r0 <*> $r1;
        BIND($r5) ::= $r0 </> $r1;
        BIND($r6) ::= $r0 <%> $r1;
        BIND($a0) ::= $r2;
        SUMMON(#1);
        BIND($a0) ::= $r3;
        SUMMON(#1);
        BIND($a0) ::= $r4;
        SUMMON(#1);
        BIND($a0) ::= $r5;
        SUMMON(#1);
        BIND($a0) ::= $r6;
        SUMMON(#1);
        HALT;
    }
}
```

### 26.11 Complete example: carry and overflow

```obl
ARCANA "flags_demo" {
    ORACLE @test_flags ~> {
        BIND($r0) ::= #-1;
        BIND($r1) ::= #1;
        BIND($r2) ::= $r0 <+> $r1;
        WHEN::CARRY => GATE @carry;
        GATE @no_carry;
        @carry::
        BIND($a0) ::= SIGIL("&eCarry set");
        SUMMON(#2);
        @no_carry::
        BIND($r0) ::= #9223372036854775807;
        INCREMENT($r0);
        WHEN::OVERFLOW => GATE @overflow;
        GATE @done;
        @overflow::
        BIND($a0) ::= SIGIL("&eOverflow set");
        SUMMON(#2);
        @done::
        HALT;
    }
}
```

### 26.12 Document status

This reference describes OBLVN 2 as implemented in `src/lang/`. If behavior and this document disagree, the implementation is authoritative, and the document should be corrected.

## 27. Bit-level cookbook

This chapter collects practical bit and address patterns. OBLVN exposes only 64-bit integers, but that is enough for bit fields, packed records, bitmaps, checksums and low-level memory layouts.

### 27.1 Bit numbering

Bit 0 is the least significant bit. Bit 63 is the most significant bit.

The mask for bit `n` is:

```text
mask = 1 << (n & 63)
```

In OBLVN:

```obl
BIND($r0) ::= #1;
BIND($r1) ::= #n;
BIND($r0) ::= $r0 <<> $r1;
```

### 27.2 Test a bit

```obl
BIND($r0) ::= #1;
BIND($r0) ::= $r0 <<> #5;
BIND($r1) ::= $r2 <&> $r0;
VERIFY($r1) <=> #0;
WHEN::EQUAL => GATE @clear;
GATE @set;
```

The value being tested is assumed to be in `$r2`. `VERIFY` against zero is the safe way to test a masked value.

### 27.3 Set a bit

```obl
BIND($r0) ::= #1;
BIND($r0) ::= $r0 <<> #5;
BIND($r1) ::= $r1 <|> $r0;
```

### 27.4 Clear a bit

```obl
BIND($r0) ::= #1;
BIND($r0) ::= $r0 <<> #5;
BIND($r0) ::= $r0 <^> #-1;
BIND($r1) ::= $r1 <&> $r0;
```

`COMPLEMENT($r0)` is equivalent to `BIND($r0) ::= $r0 <^> #-1;` and is easier to read.

### 27.5 Toggle a bit

```obl
BIND($r0) ::= #1;
BIND($r0) ::= $r0 <<> #5;
BIND($r1) ::= $r1 <^> $r0;
```

### 27.6 Extract a bit field

```obl
BIND($r0) ::= $r1;
BIND($r0) ::= $r0 <>> #8;
BIND($r0) ::= $r0 <&> #255;
```

This extracts bits 8 through 15 of the value in `$r1` as an unsigned value.

### 27.7 Insert a bit field

```obl
BIND($r0) ::= #255;
BIND($r0) ::= $r0 <<> #8;
BIND($r0) ::= $r0 <^> #-1;
BIND($r1) ::= $r1 <&> $r0;
BIND($r2) ::= $r2 <&> #255;
BIND($r2) ::= $r2 <<> #8;
BIND($r1) ::= $r1 <|> $r2;
```

`$r1` is the target word and `$r2` is the field value.

The mask clears the target field, then the shifted value is ORed in.

### 27.8 Count bits

```obl
POPCOUNT($r0);
```

Use this for Hamming weight, parity checks and coverage tests. Parity is the low bit of the popcount:

```obl
POPCOUNT($r0);
BIND($r1) ::= $r0 <&> #1;
```

### 27.9 Find the lowest set bit

```obl
TRAILING_ZEROS($r0);
```

If the value is zero, the result is 64. Use that as an explicit empty case:

```obl
TRAILING_ZEROS($r0);
VERIFY($r0) <=> #64;
WHEN::EQUAL => GATE @empty;
```

### 27.10 Find the highest set bit

```obl
LEADING_ZEROS($r0);
BIND($r1) ::= #63;
BIND($r0) ::= $r1 <-> $r0;
```

For zero, this yields `-1` because clz returns 64. Check for zero first if that matters.

### 27.11 Byte order

```obl
BYTESWAP($r0);
```

Use this when storing protocol-style big-endian integers in memory or when hashing bytes. Native memory cells are host-endian 64-bit values; byte operations always address the low 8 bits, so a consistent byte order should be chosen by the script.

### 27.12 Rotations

```obl
BIND($r0) ::= $r0 <rol> #7;
BIND($r0) ::= $r0 <ror> #7;
```

Rotations are useful for hash mixing and for circular buffers of bit fields.

### 27.13 Pack four bytes into one word

```obl
BIND($r0) ::= $a0 <&> #255;
BIND($r0) ::= $r0 <<> #24;
BIND($r1) ::= $a1 <&> #255;
BIND($r1) ::= $r1 <<> #16;
BIND($r2) ::= $a2 <&> #255;
BIND($r2) ::= $r2 <<> #8;
BIND($r3) ::= $a3 <&> #255;
BIND($r0) ::= $r0 <|> $r1;
BIND($r0) ::= $r0 <|> $r2;
BIND($r0) ::= $r0 <|> $r3;
```

### 27.14 Unpack four bytes from one word

```obl
BIND($r1) ::= $r0 <>> #24;
BIND($r1) ::= $r1 <&> #255;
BIND($r2) ::= $r0 <>> #16;
BIND($r2) ::= $r2 <&> #255;
BIND($r3) ::= $r0 <>> #8;
BIND($r3) ::= $r3 <&> #255;
BIND($r4) ::= $r0 <&> #255;
```

### 27.15 Pack coordinates

Minecraft coordinates can be negative. Masking to 8 bits stores only the low byte:

```obl
BIND($r0) ::= $a0 <&> #255;
BIND($r0) ::= $r0 <<> #24;
BIND($r1) ::= $a1 <&> #255;
BIND($r1) ::= $r1 <<> #16;
BIND($r2) ::= $a2 <&> #255;
BIND($r2) ::= $r2 <<> #8;
BIND($r3) ::= $a3 <&> #255;
BIND($r0) ::= $r0 <|> $r1;
BIND($r0) ::= $r0 <|> $r2;
BIND($r0) ::= $r0 <|> $r3;
```

This is a lossy local pack, useful for audit logs near spawn. It is not a general coordinate encoding.

### 27.16 Bitmap of 128 items

Use two 64-bit cells. The cell index is `bit / 64`, the bit position is `bit % 64`.

```obl
BIND($r0) ::= $itemId <&> #127;
BIND($r1) ::= $r0 <>> #6;
BIND($r1) ::= $r1 <+> #40;
BIND($r2) ::= #1;
BIND($r3) ::= $r0 <&> #63;
BIND($r2) ::= $r2 <<> $r3;
REVEAL($r4) ::= MEM[$r1 + #0];
BIND($r4) ::= $r4 <|> $r2;
SCRIBE MEM[$r1 + #0] ::= $r4;
```

The words live at cells 40 and 41.

### 27.17 Bitmap of 4096 bits

Use 64 cells of 64 bits each:

```text
word = bit >> 6
mask = 1 << (bit & 63)
address = base + word
```

Always validate or mask the bit index so the final address stays in range.

### 27.18 Byte array

Use `MEM8` with consecutive addresses:

```obl
BIND($r0) ::= #0;
SCRIBE8 MEM8[$r0 + #0] ::= #10;
SCRIBE8 MEM8[$r0 + #1] ::= #20;
SCRIBE8 MEM8[$r0 + #2] ::= #30;
```

Each write clears the upper bits of one 64-bit cell. Reading uses zero extension.

### 27.19 Word array

Use `MEM` with consecutive addresses:

```obl
BIND($r0) ::= #16;
SCRIBE MEM[$r0 + #0] ::= #100;
SCRIBE MEM[$r0 + #1] ::= #200;
SCRIBE MEM[$r0 + #2] ::= #300;
```

Cells are not bytes. Cell 16 is a different cell from byte 16.

### 27.20 XOR checksum

```obl
BIND($r0) ::= #0;
BIND($r1) ::= #0;
BIND($r3) ::= #0;
@loop::
REVEAL($r2) ::= MEM[$r0 + $r1];
BIND($r3) ::= $r3 <^> $r2;
INCREMENT($r1);
VERIFY($r1) <=> #32;
WHEN::LESS => GATE @loop;
```

XOR checksums are order-insensitive and cheap. Use a weighted sum if order matters.

### 27.21 FNV-like hash

OBLVN has no 32-bit overflow control beyond masking. A simple 64-bit hash:

```obl
BIND($r0) ::= #1469598103934665603;
BIND($r0) ::= $r0 <^> $r1;
BIND($r0) ::= $r0 <*> #1099511628211;
```

`$r0` is the hash and `$r1` is the next byte. The multiplication wraps. This is not cryptographic and should not be used for security.

### 27.22 Align an address

Align down to 16 cells, with the address in `$r0`:

```obl
BIND($r0) ::= $r0 <&> #-16;
```

Align up to 16 cells:

```obl
BIND($r0) ::= $r0 <+> #15;
BIND($r0) ::= $r0 <&> #-16;
```

### 27.23 Test power of two

```obl
BIND($r2) ::= $r1 <-> #1;
BIND($r2) ::= $r1 <&> $r2;
VERIFY($r2) <=> #0;
WHEN::EQUAL => GATE @power_of_two;
```

`$r1` holds the value. This is true for positive powers of two and for zero if zero is not excluded. Exclude zero first.

### 27.24 Swap two memory cells

```obl
REVEAL($r2) ::= MEM[$r0 + #0];
REVEAL($r3) ::= MEM[$r1 + #0];
SCRIBE MEM[$r0 + #0] ::= $r3;
SCRIBE MEM[$r1 + #0] ::= $r2;
```

`$r0` and `$r1` hold the two addresses.

### 27.25 Safe bit index

```obl
BIND($r0) ::= $a0 <&> #63;
```

Masking is not the same as validation. Masking folds bad indexes into range; validation rejects them:

```obl
BIND($r0) ::= $a0;
VERIFY($r0) <=> #64;
WHEN::GREATER_OR_EQUAL => GATE @bad;
```

### 27.26 Memory map discipline

Keep a table outside the script:

| Address | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | event counter |
| 1 | 1 | place counter |
| 32 | 1 | picked up items |
| 33 | 1 | last item id |
| 40 | 2 | 128-bit item bitmap |
| 64 | 16 | block break ring buffer |
| 80 | 16 | block place ring buffer |
| 96 | 1 | magic value |
| 97 | 1 | layout version |
| 200 | 1 | respawn counter |
| 201 | 1 | total damage |
| 202 | 1 | max damage |
| 203 | 1 | dropped items |

The sample scripts use this layout.

### 27.27 Ring buffer

```obl
BIND($r0) ::= #0;
REVEAL($r1) ::= MEM[$r0 + #0];
INCREMENT($r1);
SCRIBE MEM[$r0 + #0] ::= $r1;
BIND($r2) ::= $r1 <&> #15;
BIND($r3) ::= #64;
SCRIBE MEM[$r3 + $r2] ::= $r4;
```

`$r4` holds the value being written.

The index wraps with `& 15`, so writes cycle through 16 cells.

### 27.28 Bit-level debugging

Broadcast a value in decimal with host call 1 or a labeled value with host call 8:

```obl
BIND($a0) ::= SIGIL("&evalue=");
BIND($a1) ::= $r0;
SUMMON(#8);
```

There is no hexadecimal formatting host call. To inspect bits, compare masks and broadcast booleans or popcounts.

## 28. Low-level script library

The repository ships several scripts that exercise the low-level features. They are loaded automatically when `script-dir=scripts`.

### 28.1 scripts/bitpack.obl

`bitpack.obl` is an event-driven bit-packing audit script.

Features:

- initializes a magic value and layout version at addresses 96 and 97;
- packs block break events into a 16-cell ring buffer at 64..79;
- packs block place events into a ring buffer at 80..95;
- counts picked up items at 32 and records the last item id at 33;
- sets a bit in a 128-bit item bitmap at cells 40..41;
- computes an XOR checksum over 32 buffer cells every 600 script ticks;
- uses `POPCOUNT` and `BYTESWAP` on the checksum;
- broadcasts the results with host call 8;
- logs the block break count at shutdown with host call 7.

No chat message is sent for normal block events. The checksum message appears once per 600 script ticks, which is every 10 minutes with the default interval.

Example packing:

```obl
BIND($r0) ::= $a0 <&> #255;
BIND($r0) ::= $r0 <<> #24;
```

Coordinates are packed as four bytes in one 64-bit cell.

### 28.2 scripts/bitwise.obl

`bitwise.obl` is a set of non-event test oracles.

`test_bitwise` demonstrates:

- `POPCOUNT`;
- `LEADING_ZEROS`;
- `TRAILING_ZEROS`;
- `BYTESWAP`;
- rotate left and rotate right.

`test_bitfields` demonstrates:

- creating a mask with a shift;
- setting a bit with OR;
- testing a bit with AND and `VERIFY`;
- clearing a bit with AND and complement;
- toggling a bit with XOR.

Run them manually:

```text
/script run test_bitwise
/script run test_bitfields
```

They are not event oracles, so they do not run on server events.

### 28.3 scripts/memory_map.obl

`memory_map.obl` is an event-driven memory map example.

Event handlers:

- `on_respawn` increments the respawn counter at 200;
- `on_player_damage` adds damage to 201 and tracks the maximum at 202;
- `on_item_drop` adds dropped counts to 203;
- `test_table` fills 16 cells with an arithmetic sequence and sums them with an index register.

Run:

```text
/script run test_table
```

This broadcasts a labeled sum with host call 8.

### 28.4 scripts/syntax_test.obl

`syntax_test.obl` is the compile-checked syntax showcase. It includes every statement family, including the new bit operations and rotate operators.

It has no event oracles. Run oracles manually:

```text
/script run arithmetic
/script run memory
/script run conditions
/script run calls
/script run host_calls
```

`trap_form` intentionally traps if run.

### 28.5 Relationship to server events

These files use the following server events:

| Event | Example file |
| --- | --- |
| `on_server_start` | `bitpack.obl` |
| `on_server_stop` | `bitpack.obl` |
| `on_block_break` | `bitpack.obl` |
| `on_block_place` | `bitpack.obl` |
| `on_item_pickup` | `bitpack.obl` |
| `on_item_drop` | `memory_map.obl` |
| `on_player_damage` | `memory_map.obl` |
| `on_respawn` | `memory_map.obl` |
| `on_join` | `welcome.obl` |
| `on_chat` | none by default |
| `on_tick` | `stats.obl`, `bitpack.obl` |

Adding a file with an event oracle makes that oracle part of the running server after the next reload.

### 28.6 Extending the library

When adding a low-level script:

1. Choose a non-overlapping memory map.
2. Define event oracles only for intentional behavior.
3. Keep chat broadcasts rare.
4. Validate or mask indexes before memory access.
5. Use `MEM8` for byte arrays and `MEM` for word arrays.
6. Use host call 7 for audit logs and host call 8 for labeled summaries.
7. Test with `/script run` first.
8. Reload and check the load count.

### 28.7 Full memory map of the sample scripts

| Address | Owner |
| --- | --- |
| 0 | `bitpack.obl` block break counter |
| 1 | `bitpack.obl` block place counter |
| 32 | `bitpack.obl` picked up item count |
| 33 | `bitpack.obl` last item id |
| 40..41 | `bitpack.obl` item bitmap |
| 64..79 | `bitpack.obl` block break ring |
| 80..95 | `bitpack.obl` block place ring |
| 96 | `bitpack.obl` magic |
| 97 | `bitpack.obl` version |
| 200 | `memory_map.obl` respawn counter |
| 201 | `memory_map.obl` total damage |
| 202 | `memory_map.obl` maximum damage |
| 203 | `memory_map.obl` dropped items |

Each file has its own memory, so these addresses do not overlap across files. They would overlap if merged into one file.
