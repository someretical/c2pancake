# c2pancake

C to [Pancake](https://cakeml.org/pancake.html) transpiler. Pancake is a new 'thin' language constructed with the aims of (1) ease of source-level verification, (2) user-controlled small memory footprint, and (3) simple transportation of correctness results from source to executable binary code.

```
USAGE: c2pancake [options] <source0> [... <sourceN>]

OPTIONS:

Generic Options:

  --help                      - Display available options (--help-hidden for more)
  --help-list                 - Display list of available options (--help-list-hidden for more)
  --version                   - Display the version of this program

c2pancake options:

  --cake-options=<string>     - Override Cake compiler options (default: --pancake --main_return=true)
  --cake-path=<string>        - Path to the Cake compiler (default: cake in PATH)
  --end=<string>              - End the pipeline after this pass (including retries) (default: empty, meaning end after last pass)
  --extra-arg=<string>        - Additional argument to append to the compiler command line
  --extra-arg-before=<string> - Additional argument to prepend to the compiler command line
  --make-path=<string>        - Override make path (default: make)
  --max-pass-retries=<ulong>  - Maximum number of retries for a pass before moving to the next pass (default: 20)
  -p <string>                 - Build path
  --pointer-bits=<value>      - Override pointer width
    =32                       -   32-bit
    =64                       -   64-bit
  --skip=<string>             - Passes to skip (default: empty, meaning no passes are skipped)
  --stack-size=<ulong>        - Override Pancake stack size (default: 0x10000 = 4096 * 16)
  --start=<string>            - Start the pipeline at this pass (default: empty, meaning start at beginning)

-p <build-path> is used to read a compile command database.

        For example, it can be a CMake build directory in which a file named
        compile_commands.json exists (use -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        CMake option to get this output). When no build path is specified,
        a search for compile_commands.json will be attempted through all
        parent paths of the first input file . See:
        https://clang.llvm.org/docs/HowToSetupToolingForLLVM.html for an
        example of setting up Clang Tooling on a source tree.

<source0> ... specify the paths of source files. These paths are
        looked up in the compile command database. If the path of a file is
        absolute, it needs to point into CMake's source tree. If the path is
        relative, the current working directory needs to be in the CMake
        source tree and the file must be in a subdirectory of the current
        working directory. "./" prefixes in the relative files will be
        automatically removed, but the rest of a relative path must be a
        suffix of a path in the compile command database.

Documentation:
  https://github.com/someretical/c2pancake
```

## Examples

```
c2pancake tests/arith.c --skip ConvertIntegerTypes --
```

Note if see a warning in the output like `WARNING: Maximum number of passes (X) reached, consider increasing the limit`, use `--max-pass-retries=` option as described above!

## Restrictions

### Before running the tool

1. Remove any static symbols within functions.
1. Rewrite any switch statements with loops inside them. This is problematic because the loops can have case statements inside them which cannot be correctly transpiled.
1. Switch statement cases should be rewritten so they don't contain any `break;`s in the middle of cases (i.e. `break`'s should only appear at the end of a case statement). Also, case statements are only allowed at the top level scope in the switch statement (i.e. don't nest them inside of `{}`'s).
1. Static inline functions in headers should be placed in a special header file. Then run the preprocessor with args `-nostdinc -I/path/to/header` to ONLY process the special header file. This will ensure the functions are inlined into the source file and thus processed by c2pancake. Don't include anything that shouldn't be converted (e.g. C library functions and #defines).
1. Remove all variadic functions defined within the source file along with calls to them.
1. Remove all variable length arrays include incomplete array members within records (structs/unions).
1. If there are any function declarations that should not be rewritten to follow Pancake's calling convention, move them into header files.
1. If there are any function definitions that should not be rewritten to follow Pancake's calling convention, annotate them with `[[clang::annotate("__c2pnk_external_entry_point")]]`.
    - E.g. the [`init`](https://github.com/seL4/microkit/blob/main/docs/manual.md#entry-points) function in [sDDF](https://github.com/au-ts/sDDF) has the signature `void init(void)` and applications are expected to implement it.
1. Global variables shared across threads should be annotated with `[[clang::annotate("__c2pnk_shared_global")]]`.
1. Global variables that are loaded externally should be annotated with `[[clang::annotate("__c2pnk_external_global")]]`. Their types should also be modified to be a pointer
1. Macros like `assert()` cannot be converted properly so they should be expanded beforehand or replaced with a function.

### Other restrictions

1. All arrays and structs in functions are considered "static" (but not shareable across threads) so no recursion is allowed. They will be hoisted into the global scope with name mangling.
1. Any stack variable which has its address taken will also be hoisted into the global scope with name mangling.
1. Floating point types are not supported at all.
1. Function argument evaluation order will NOT be preserved. Complicated args requiring hoisting of variables will be evaluated first while simpler expressions will be left as-is.

## Development

The instructions below are for Ubuntu 24.x.

At a high level
- LLVM 22.1.8 (use later versions at risk of breaking compatibility...)
- GCC 16 and libstdc++16 (clang's libc++ doesn't support enough modern C++(26) features)
- Python 3.12.3 (for fetching clang internal headers from GitHub)
- at LEAST 24GB of RAM if using WSL + VSCode and building locally 💀
  - for those at Trustworthy Systems working on this project, message Yankai for the script to build on dedicated infrastructure :-)

### Install LLVM + libstdc++ + other build tools

Install LLVM development dependencies first
```
sudo apt update
sudo apt install libedit-dev zlib1g-dev libzstd-dev libcurl4-openssl-dev -y
```

Install LLVM development libraries, current supported version is 22.1.8. N.B. it is not possible to specify the minor and patch versions.
```
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22 all
```

Install GCC/libstdc++ 16
```
sudo apt install software-properties-common -y
sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y
sudo apt update
sudo apt install gcc-16 g++-16 libstdc++-16-dev -y
```

Install other build tools
```
sudo apt install build-essential ninja-build cmake cmake-format python3 python3-venv -y
```

The clang internal headers for LLVM 22.1.8 are already included. If targeting a newer release (even if it is a minor release), run the helper python script to fetch the newer headers.
```
/usr/bin/python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python fetch_codegen_headers.py --version llvmorg-22.1.8
```

### Installing pancake compiler

Head to https://cakeml.org/regression.cgi and find the latest successful job on master.

Copy the download link for the artefacts.

```
mkdir build && cd build && wget -c <LINK> -O - | tar -xz
```
E.g.
```
mkdir build && cd build && wget -c https://cakeml.org/regression/artefacts/3389/cake-x64-64.tar.gz -O - | tar -xz
```

### VSCode extensions

Install from command line
```
code --install-extension \
  ms-python.black-formatter \
  ms-python.debugpy \
  ms-python.isort \
  ms-python.python \
  ms-python.vscode-pylance \
  ms-python.vscode-python-envs \
  llvm-vs-code-extensions.lldb-dap \
  llvm-vs-code-extensions.vscode-clangd \
  ms-vscode.cmake-tools \
  ms-vscode.cpp-devtools \
  ms-vscode.cpptools \
  cheshirekow.cmake-format
```

### CMake

Easiest method is to use the VSCode CMake extension.

Manual instructions (make sure you are in the project folder):
```
cmake \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm-22/lib/cmake/clang \ 
  -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
  -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang-22 \
  -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-22 \
  --no-warn-unused-cli \
  -S . \
  -B build \
  -G "Ninja Multi-Config"
```

Debug build:
```
cmake --build build --config Debug --target all --
```

Release build:
```
cmake --build build --config Release --target all --
```

### Other commands

Dump Clang AST
```
clang-22 -fsyntax-only -Xclang -ast-dump-all <file.c>
```

Test `uart.c`
```
./build/Debug/c2pancake /home/one/sddf/drivers/serial/imx/uart.c -- \
  -ffreestanding -mstrict-align -mcpu=cortex-a53 -mtune=cortex-a53 -target aarch64-none-elf \
  -I/home/one/microkit_tutorial/microkit-sdk-2.2.0/board/maaxboard/debug/include \
  -I/home/one/sddf/examples/serial/include \
  -I/home/one/sddf/include \
  -I/home/one/sddf/include/microkit \
  -I/home/one/sddf/include/sddf/util/custom_libc \
  -I/home/one/sddf/drivers/serial/imx/include \
  -I/usr/lib/gcc-cross/aarch64-linux-gnu/13/include
```

## Detailed pass documentation

The pipeline is registered in [`main.cpp`](./src/main.cpp) and runs in the following order. Most AST-rewrite passes:

- inspect the current translation unit;
- add source replacements;
- return `Continue` when no rewrite is needed;
- return `Repeat` when replacements were made, causing the same pass to run again until stable;
- return `NextFile` on fatal transformation errors.

The pipeline itself is managed by [`Pipeline.cpp`](./src/Pipeline.cpp), which creates each intermediate source file as `<input>.<pass>-<attempt>-c2pnk.c`.

### `pass_inject_headers`

Implemented in [`Pass_InjectHeaders.cpp`](./src/Pass_InjectHeaders.cpp).

Ensures that the input contains:

```c
#include <stdint.h>
```

It uses Clang's `HeaderIncludes` utility, so it avoids inserting a duplicate include and places the include according to the detected include style. This header is required by generated helper functions and the Pancake representation of integer values.

---

### `pass_name_anon_records`

Implemented in [`Pass_PromoteRecords.cpp`](./src/Pass_PromoteRecords.cpp).

Gives names to anonymous record definitions in the main source file.

For example:

```c
struct {
    int x;
} value;
```

becomes conceptually:

```c
struct __c2pnk_anon_record_L1C1 {
    int x;
} value;
```

The generated name includes the source line and column. The pass deliberately avoids naming anonymous structs whose fields are injected into an enclosing record or union, because naming those records could alter C's field lookup behavior.

---

### `pass_rename_to_be_promoted_records`

Implemented in [`Pass_PromoteRecords.cpp`](./src/Pass_PromoteRecords.cpp).

Renames record definitions declared inside functions or other non-top-level contexts.

A local record such as:

```c
void f(void) {
    struct Local {
        int x;
    };
}
```

is renamed to a globally unique name such as:

```c
struct __c2pnk_promoted_record_Local_L2C5 {
    int x;
};
```

All explicit references to the original record declaration are renamed using Clang's reference-finding facilities. This prepares the record for the next pass, which moves its definition to function scope-independent storage.

---

### `pass_promote_records`

Implemented in [`Pass_PromoteRecords.cpp`](./src/Pass_PromoteRecords.cpp).

Moves local record definitions into the beginning of their containing function.

The pass:

1. Finds record definitions declared inside functions.
2. Removes the inline record definition from its original declaration statement.
3. Inserts the complete record definition before the function body.
4. Leaves only a normal declaration or use at the original location.

This removes nested record declarations, which are inconvenient for Pancake's simpler type and declaration model, while preserving the record's function-local visibility as much as possible.

---

### `pass_inject_memcpy_polyfill`

Implemented in [`Pass_FunctionCalling.cpp`](./src/Pass_FunctionCalling.cpp).

Injects a generated byte-by-byte implementation of `memcpy`:

```c
static inline uintN_t __c2pnk_memcpy(
    uint8_t *dest,
    uint8_t *src,
    uintN_t len
);
```

The width of `uintN_t` matches the configured pointer width. The helper copies bytes in a loop and returns zero, matching the calling convention expected by later lowering passes.

It is inserted at the beginning of the file before function-calling rewrites are performed.

---

### `pass_function_calling`

Implemented in [`Pass_FunctionCalling.cpp`](./src/Pass_FunctionCalling.cpp).

Rewrites C functions and calls to conform to Pancake's restricted calling convention.

Its responsibilities include:

- rewriting non-FFI function definitions and declarations;
- rewriting calls to rewritten functions;
- converting `void` returns into the integer return form expected by Pancake;
- passing aggregate parameters indirectly through generated storage;
- returning aggregate values through generated out-parameters or global/hoisted storage;
- generating wrappers for FFI functions;
- handling variadic FFI calls separately;
- preserving external entry points marked with `__c2pnk_external_entry_point`;
- processing configured extra headers for FFI wrappers;
- rejecting unsupported variadic non-FFI functions;
- rejecting integer or pointer parameters and return values wider than the configured pointer width.

FFI rewrites are deliberately applied before non-FFI rewrites. If the two categories would interfere, the pass requests another pipeline attempt so that the FFI transformations become visible first.

---

### `pass_normalise_while_loops`

Implemented in [`Pass_LoopsToWhile.cpp`](./src/Pass_LoopsToWhile.cpp).

Moves a `while` condition into the body and changes the loop condition to the constant word-sized value `1UL`.

For example:

```c
while (condition) {
    body;
}
```

becomes:

```c
while (1UL) {
    if (!(condition)) {
        break;
    }
    body;
}
```

This gives every while loop a uniform infinite-loop shape with an explicit conditional break, which is easier for subsequent lowering.

A loop already written as `while (1)` is left unchanged.

---

### `pass_process_continue_in_for_loops`

Implemented in [`Pass_LoopsToWhile.cpp`](./src/Pass_LoopsToWhile.cpp).

Prepares `continue` statements for conversion of `for` loops into `while` loops.

In C, a `continue` in a `for` loop executes the loop increment before starting the next iteration. A direct conversion to `while` would otherwise skip that increment.

Thus:

```c
for (init; cond; increment) {
    if (x)
        continue;
    body;
}
```

is changed conceptually to:

```c
for (init; cond; increment) {
    if (x) {
        increment;
        continue;
    }
    body;
}
```

The pass only rewrites continues directly owned by the matched `for` loop; continues belonging to nested loops are excluded.

---

### `pass_for_to_while`

Implemented in [`Pass_LoopsToWhile.cpp`](./src/Pass_LoopsToWhile.cpp).

Converts every `for` loop into a block containing a `while (1UL)` loop.

The general form is:

```c
for (init; condition; increment) {
    body;
}
```

to:

```c
{
    init;
    while (1UL) {
        if (!(condition)) {
            break;
        }

        body;

        increment;
    }
}
```

The initializer, condition, and increment are optional. Separate rewrite rules handle compound and single-statement loop bodies.

---

### `pass_process_continue_in_do_while_loops`

Implemented in [`Pass_LoopsToWhile.cpp`](./src/Pass_LoopsToWhile.cpp).

Preserves the special behavior of `continue` in a `do ... while` loop.

A `continue` in a do-while loop must evaluate the loop condition before deciding whether to continue. The pass therefore changes:

```c
do {
    body1;
    continue;
    body2;
} while (condition);
```

conceptually into:

```c
while (1UL) {
    body1;
    if (condition) {
        continue;
    }
    body2;
    if (!(condition)) {
        break;
    }
}
```

Nested-loop continues are not rewritten by the outer loop's pass.

---

### `pass_do_while_to_while`

Implemented in [`Pass_LoopsToWhile.cpp`](./src/Pass_LoopsToWhile.cpp).

Converts `do ... while` loops into `while (1UL)` loops while preserving their guaranteed first iteration.

Conceptually:

```c
do {
    body;
} while (condition);
```

becomes:

```c
while (1UL) {
    body;
    if (!(condition)) {
        break;
    }
}
```

This completes the normalization of all supported C loop forms into explicit while loops.

---

### `pass_add_switch_fallthrough`

Implemented in [`Pass_SwitchToIf.cpp`](./src/Pass_SwitchToIf.cpp).

Adds an explicit `[[fallthrough]];` marker to nested case labels.

The pass identifies a case label whose parent is another case label and inserts the attribute before it. This makes intentional fallthrough explicit in the AST, allowing the following switch-normalization pass to distinguish fallthrough labels from independent case bodies.

---

### `pass_normalise_switches`

Implemented in [`Pass_SwitchToIf.cpp`](./src/Pass_SwitchToIf.cpp).

Normalizes switch cases into a predictable representation before converting switches to if-statements.

It:

- visits inner switches before outer switches;
- groups consecutive case labels;
- recognizes `break`, `return`, and `continue` as terminating statements;
- propagates the previous case body into fallthrough cases where necessary;
- handles `default` labels like ordinary case labels;
- removes or relocates case-body structure so each case can be represented as one explicit block;
- validates that the resulting switch body contains only supported case/default forms.

The implementation understands the `[[fallthrough]]` markers inserted by `pass_add_switch_fallthrough`.

---

### `pass_switch_to_if`

Implemented in [`Pass_SwitchToIf.cpp`](./src/Pass_SwitchToIf.cpp).

Converts normalized `switch` statements into chains of ordinary `if` statements.

A switch such as:

```c
switch (x) {
case 1:
    a();
    break;
case 2:
    b();
    break;
default:
    c();
    break;
}
```

is lowered conceptually to:

```c
if (x == 1) {
    a();
} else if (x == 2) {
    b();
} else {
    c();
}
```

The pass handles grouped labels, default cases, fallthrough-expanded bodies, and switch expressions whose evaluation must be preserved. It also removes switch-specific control-flow constructs that Pancake does not support directly.

---

### `pass_normalise_if_statements`

Implemented in [`Pass_NormaliseIfStatements.cpp`](./src/Pass_NormaliseIfStatements.cpp).

Normalizes conditional statements so that their bodies and conditions have explicit, predictable forms.

It:

- adds braces around single-statement `then` and `else` branches;
- ensures an `else if` chain remains syntactically valid after rewriting;
- hoists complex conditions into temporary variables;
- leaves simple variable conditions alone where possible;
- gives generated temporaries stage- and attempt-specific names.

For example:

```c
if (a + b) {
    f();
} else
    g();
```

becomes conceptually:

```c
word_t __c2pnk_if_cond_... = (a + b);
if (__c2pnk_if_cond_...) {
    f();
} else {
    g();
}
```

---

### `pass_hoist_condition_expressions`

Implemented in [`Pass_TransformLogicalExpressions.cpp`](./src/Pass_TransformLogicalExpressions.cpp).

Hoists side-effecting `if` conditions into temporaries.

For example:

```c
if (f() && x++) {
    body();
}
```

becomes conceptually:

```c
word_t __c2pnk_If_... = f() && x++;
if (__c2pnk_If_...) {
    body();
}
```

This makes the condition's evaluation explicit and prevents later transformations from duplicating or reordering side effects.

---

### `pass_rewrite_array_indexing`

Implemented in [`Pass_TransformLogicalExpressions.cpp`](./src/Pass_TransformLogicalExpressions.cpp).

Rewrites C array subscripting into pointer arithmetic and dereferencing.

```c
array[index]
```

becomes:

```c
(*(array + (index)))
```

This makes array access use the same primitive pointer operations that the Pancake backend understands.

---

### `pass_rewrite_struct_stabs`

Implemented in [`Pass_TransformLogicalExpressions.cpp`](./src/Pass_TransformLogicalExpressions.cpp).

Rewrites pointer-member access from `->` syntax into explicit dereference and member access.

```c
pointer->field
```

becomes:

```c
(*(pointer)).field
```

This removes another C-specific convenience operator and exposes the underlying pointer and record operations.

---

### `pass_lower_nested_expressions`

Implemented in [`Pass_TransformLogicalExpressions.cpp`](./src/Pass_TransformLogicalExpressions.cpp).

This is one of the largest expression-lowering passes.

It recursively converts complicated C expressions and statements into simpler expressions with explicit temporary variables. It tracks whether an expression is needed as:

- a value;
- a place/storage location;
- an effect only.

It handles, among other constructs:

- assignments and compound assignments;
- comma expressions;
- conditional expressions;
- logical `&&` and `||`;
- pre/post increment and decrement;
- calls;
- dereferences and address-of expressions;
- member access;
- statement expressions;
- initializer lists and compound literals;
- pointer arithmetic;
- nested binary and unary expressions.

The pass emits prerequisite statements before the final expression. This makes evaluation order explicit and ensures that expressions with side effects are evaluated once.

GNU statement expressions require repeated execution of this pass because their internal statements are not fully traversed during the initial rewrite.

---

### `pass_simplify_double_negation`

Implemented in [`Pass_TransformLogicalExpressions.cpp`](./src/Pass_TransformLogicalExpressions.cpp).

Simplifies:

```c
!!expression
```

to:

```c
expression
```

At this stage, earlier normalization has already converted the relevant boolean representation so that the simplification is valid for the generated form.

---

### `pass_lower_bitfield_ops`

Implemented in [`Pass_LowerBitfieldOps.cpp`](./src/Pass_LowerBitfieldOps.cpp).

Lowers C bit-field reads and writes into explicit byte and bit operations.

It computes:

- the byte address containing the field;
- the starting and ending bit positions;
- masks and shifts;
- sign extension for signed bit-fields;
- truncation when storing values.

For non-volatile fields it injects helper functions such as:

```c
__c2pnk_get_bitfield_uN
__c2pnk_get_bitfield_iN
__c2pnk_set_bitfield_uN
__c2pnk_set_bitfield_iN
```

For volatile fields it preserves the required individual memory accesses rather than replacing them with an unsafe combined operation.

The generated helpers use the configured pointer width.

---

### `pass_simplify_addrof_deref`

Implemented in [`Pass_LowerBitfieldOps.cpp`](./src/Pass_LowerBitfieldOps.cpp).

Simplifies the identity:

```c
&*expression
```

to:

```c
expression
```

This removes redundant address/dereference pairs introduced by earlier lowering passes.

---

### `pass_implicit_to_explicit_casts`

Implemented in [`Pass_IntegerConversion.cpp`](./src/Pass_IntegerConversion.cpp).

Finds implicit casts inserted by Clang and makes the supported conversions explicit in the source.

For example, an implicit conversion to `uint32_t` becomes conceptually:

```c
(uint32_t)(expression)
```

The pass intentionally excludes conversions that are already represented by pointer decay, function decay, lvalue-to-rvalue conversion, or no-op casts.

This pass is run twice. The second invocation is intentional: the integer-conversion pass can expose additional implicit casts, and the final explicit casts are needed to keep the generated C source warning-free even though they do not affect Pancake lowering.

---

### `pass_integer_conversion`

Implemented in [`Pass_IntegerConversion.cpp`](./src/Pass_IntegerConversion.cpp).

Converts C integer operations into Pancake's word-sized integer representation.

Pancake stores integer values in one machine word. The pass maintains the following representation:

- unsigned `N`-bit values are zero-extended into the low bits;
- signed `N`-bit values are sign-extended to the full word width.

The pass rewrites:

- integer literals;
- character literals;
- integer declarations and initializers;
- integer casts;
- signed and unsigned conversions;
- arithmetic;
- comparisons;
- shifts;
- unary negation and bitwise operations;
- loads and stores;
- integer-valued member expressions;
- function-call results involving integers.

It injects width-specific helper functions for operations that cannot be represented directly by ordinary C expressions, including truncation, sign extension, signed comparisons, and signed arithmetic.

The implementation supports either 32-bit or 64-bit pointer-width operation and rejects unsupported floating-point cases.

**This pass is currently recommended to be disabled. Pancake does not yet support inlining the result of a function call as a parameter to another function call. This is something that the integer conversion code will generate quite frequently due to the default C `int` type being signed!**

---

### `pass_implicit_to_explicit_casts` — second invocation

This is the same pass as number 23, deliberately registered again in [`main.cpp`](./src/main.cpp).

The first integer-conversion pass can create new AST conversions. Running the explicit-cast pass again ensures those newly exposed conversions are visible in the final C source.

---

### `pass_rename_to_be_hoisted_globals`

Implemented in [`Pass_HoistArraysAndAddresses.cpp`](./src/Pass_HoistArraysAndAddresses.cpp).

Finds function-local objects that must be moved out of the stack-like local scope.

It marks:

- local arrays;
- local record or union objects;
- local variables whose address is taken.

Each such variable is renamed consistently at every explicit reference using a generated name containing:

- the original variable name;
- the containing function;
- source line and column;
- pipeline stage and attempt number.

The pass also rejects static variables declared inside functions, since those are unsupported by the intended Pancake representation.

It does not itself move the declarations; it prepares names so the following hoisting pass can do so safely.

---

### `pass_hoist_locals`

Implemented in [`Pass_HoistArraysAndAddresses.cpp`](./src/Pass_HoistArraysAndAddresses.cpp).

Moves the variables renamed by the previous pass into function prologues.

For each generated local:

1. A declaration is inserted at the beginning of the containing function.
2. The original declaration is removed.
3. Any initializer is converted into an assignment at the original location.

For example:

```c
void f(void) {
    int array[4] = {0};
    use(array);
}
```

becomes conceptually:

```c
int __c2pnk_local_array_... [4];

void f(void) {
    __c2pnk_local_array_... = ...;
    use(__c2pnk_local_array_...);
}
```

The exact generated form depends on the type and preceding rewrites. The purpose is to ensure arrays, records, and address-taken locals are no longer represented as ordinary stack locals.

---

### `pass_c2pancake`

Implemented in [`Pass_C2Pancake.cpp`](./src/Pass_C2Pancake.cpp).

This is the final AST-to-Pancake translation pass.

It traverses the normalized C AST and builds an internal Pancake program representation. It translates:

- global variables;
- functions;
- parameters and return values;
- expressions;
- assignments;
- pointer operations;
- conditionals;
- loops;
- calls;
- returns;
- record layout and field offsets;
- bit-width and alignment information;
- external globals;
- generated FFI wrappers.

It distinguishes three expression contexts:

- `Value`: the expression's result is needed;
- `Place`: the expression identifies storage to read or write;
- `Effect`: only side effects are needed.

The pass also:

- assigns addresses to global objects;
- computes heap size;
- emits field-offset macros where needed;
- collects generated FFI source;
- converts the internal Pancake program to textual Pancake output;
- publishes translation-unit, FFI, global-variable, and heap-size artifacts.

Its finalizer writes:

- `<output>.pancake`;
- `<output>.scaffold.c`;
- `<output>.ffi.c`;
- `<output>.makefile`.

The scaffold initializes Pancake heap and stack memory, initializes externally declared globals, calls `cml_main`, and provides the C entry point. This final stage is therefore both the compiler backend and the output-file generator.
