# Clang C++ Extensions — Experimental Language Features

This fork of [llvm/llvm-project](https://github.com/llvm/llvm-project) adds three experimental C++ language extensions to Clang 23:

- **C++26 Reflection** (`^^` operator, P2996)
- **C++ Pattern Matching** (`match` expression, P2688-inspired)
- **C++26 Contracts** (`contract_assert` statement, P2900-inspired)

> ⚠️ **These are experimental extensions.** They are not part of any C++ standard and may change at any time. They are intended for experimentation and prototyping only.

---

## Building

```bash
# Clone
git clone https://github.com/tonytranrp/llvm-project.git
cd llvm-project

# Configure (same as upstream Clang)
cmake -G Ninja -S llvm -B ../llvm-build \
  -DLLVM_ENABLE_PROJECTS="clang;lld;clang-tools-extra" \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_BUILD_TESTS=OFF

# Build
cd ../llvm-build && ninja -j$(nproc)
```

The resulting `bin/clang++` supports the three new flags.

---

## Feature 1: C++26 Reflection (`-freflection`)

Implements the `^^` reflection operator from [P2996](https://wg21.link/p2996). Produces a value of type `std::meta::info`.

### Enabled with

```
clang++ -std=c++26 -freflection
```

### Syntax

```cpp
^^type-id          // Reflect a type:       ^^int, ^^MyClass, ^^std::vector<int>
^^id-expression    // Reflect a declaration: ^^my_var, ^^my_function
^^::               // Reflect the global namespace
```

### Examples

```cpp
// Reflect a built-in type
auto r1 = ^^int;              // type: std::meta::info

// Reflect a user-defined type
struct Point { int x, y; };
auto r2 = ^^Point;            // type: std::meta::info

// Reflect the global namespace
auto r3 = ^^::;               // type: std::meta::info
```

### Current Limitations

- `auto` cannot deduce `std::meta::info` (it's a placeholder type). Use explicit types or discard the result.
- **Code generation is not yet supported** — reflection expressions work in parse/sema but will crash during LLVM IR emission. Use `-fsyntax-only` for now.
- No metafunctions yet (`is_type`, `type_of`, `identifier_of`, etc.) — planned for Tier 2.
- `^^namespace_name` (non-global namespace) is not yet supported.

---

## Feature 2: C++ Pattern Matching (`-fpattern-matching`)

Implements a `match` expression inspired by [P2688](https://wg21.link/p2688). Produces a value based on pattern matching.

### Enabled with

```
clang++ -std=c++26 -fpattern-matching
```

### Syntax

```cpp
match(scrutinee) {
    pattern1 => result1,
    pattern2 => result2,
    _         => default_result
}
```

- **Patterns**: Integer literals, boolean expressions, and the wildcard `_` (matches anything).
- **Guard clauses**: `pattern if condition => result` (parsed but not yet used in lowering).
- **Result type**: All result expressions must have the same type.

### Examples

```cpp
// Basic pattern matching
int classify(int x) {
    return match(x) {
        0 => 1,
        1 => 10,
        _ => 100
    };
}

// With wildcard
const char* day_type(int d) {
    return match(d) {
        0 => "Sunday",
        6 => "Saturday",
        _ => "Weekday"
    };
}
```

### How It Works

Pattern matching is **lowered to a chain of conditional expressions** (`?:`):

```cpp
match(x) { 0 => 1, _ => 2 }
// lowers to:
(x == 0) ? 1 : 2
```

Wildcard patterns (`_`) become `true`, so the final wildcard arm is unconditional.

### Current Limitations

- Only expression patterns (literals, values) and wildcards — no destructuring, no type patterns.
- Guard clauses (`if condition`) are parsed but not yet used in lowering.
- No exhaustiveness checking.
- Identifier binding patterns are not yet functional.

---

## Feature 3: C++26 Contracts (`-fcontracts`)

Implements `contract_assert` statements inspired by [P2900](https://wg21.link/p2900).

### Enabled with

```
clang++ -std=c++26 -fcontracts
```

### Syntax

```cpp
contract_assert(condition);
contract_assert(condition, "message");
```

### Examples

```cpp
int safe_divide(int a, int b) {
    contract_assert(b != 0);
    return a / b;
}

void process(int* ptr) {
    contract_assert(ptr != nullptr, "null pointer passed to process");
    // ...
}
```

### How It Works

For the MVP, `contract_assert(cond)` is lowered to the condition evaluated as a statement. The condition is checked at runtime — if it evaluates to `false`, the behavior is undefined (like standard `assert()` without the abort).

### Current Limitations

- No `if-then-trap` lowering yet — the condition is evaluated but a failure does not call `__builtin_trap()`.
- `contract_assert(cond)` may produce a `-Wunused-comparison` warning when the condition is a comparison expression (e.g., `contract_assert(b != 0)`). This is because the MVP lowering evaluates the condition as a discarded-value expression. The Tier 2 `if-then-trap` lowering will eliminate this warning.
- No `pre`/`post` contract annotations on function declarations.
- No contract violation handler customization.
- No contract side-effect analysis.

---

## Combining Features

All three flags can be used together:

```bash
clang++ -std=c++26 -freflection -fpattern-matching -fcontracts my_code.cpp
```

---

## Implementation Details

### Files Modified (34 files total)

| Category | Files |
|----------|-------|
| **Lexer** | `TokenKinds.def`, `Lexer.cpp` — `^^` and `=>` tokens |
| **Parser** | `Parser.h`, `ParseExpr.cpp`, `ParseStmt.cpp`, `ParseReflect.cpp`, `ParsePatternMatching.cpp` (new) |
| **AST** | `ExprCXX.h`, `ExprCXX.cpp`, `BuiltinTypes.def`, `ASTContext.h`, `ASTContext.cpp`, `Type.cpp`, `TypeLoc.cpp`, `TypeBase.h` — `CXXReflectExpr` + `MetaInfo` type |
| **Sema** | `Sema.h`, `SemaExpr.cpp`, `SemaPatternMatching.cpp` (new) |
| **CodeGen** | `CodeGenTypes.cpp` — `MetaInfo` → `i64` mapping, `CGExprScalar.cpp` — `CXXReflectExpr` emission |
| **Diagnostics** | `DiagnosticSemaKinds.td` |
| **LangOpts** | `LangOptions.def`, `IdentifierTable.h`, `IdentifierTable.cpp` |
| **Driver** | `Clang.cpp`, `Options.td` — `-freflection`, `-fpattern-matching`, `-fcontracts` |
| **Serialization** | `ASTBitCodes.h`, `ASTCommon.cpp`, `ASTReader.cpp` — `MetaInfo` type support |
| **Other** | `NSAPI.cpp`, `StmtNodes.td`, `CMakeLists.txt` (Parse, Sema) |

### Architecture

- **Reflection**: `^^` lexed as `tok::caretcaret`, parsed in `ParseReflect.cpp`, produces `CXXReflectExpr` with `ReflectionKind` (RK_Type, RK_Declaration, RK_GlobalNamespace). Result type is `std::meta::info` (a builtin type mapped to `i64` in LLVM IR). `auto` deduction works: `auto r = ^^int;` deduces to `std::meta::info`.
- **Pattern Matching**: `match` is a keyword (gated by `LangOpts.PatternMatching`), `=>` is `tok::equalgreater`. Parsed in `ParsePatternMatching.cpp`, lowered to ternary chains in `SemaPatternMatching.cpp`.
- **Contracts**: `contract_assert` is a keyword (gated by `LangOpts.Contracts`). Parsed in `ParseStmt.cpp`, lowered to expression statement in `SemaPatternMatching.cpp`.
- **Keyword sharing**: `match` and `contract_assert` share the `KEYPATTERNMATCHING` bit (0x80000000) since they never conflict — enabled by their respective LangOpts.

---

## Roadmap

### Tier 2 (Next)
- [ ] Reflection metafunctions: `is_type()`, `type_of()`, `identifier_of()`, `members_of()`
- [ ] Reflection CodeGen — emit meaningful `i64` values (type indices, decl pointers) instead of `0`
- [ ] Pattern matching destructuring: `auto [x, y]` patterns
- [ ] Pattern matching guard lowering: `pattern if guard => result`
- [ ] Contracts `if-then-trap` lowering with `__builtin_trap()`
- [ ] Contracts `pre`/`post` on function declarations
- [ ] `^^namespace_name` support

### Tier 3 (Future)
- [ ] Reflection on templates and concepts
- [ ] Pattern matching exhaustiveness checking
- [ ] Contract violation handlers
- [ ] AST pretty-printing for new expression types

---

## License

Same as upstream LLVM: Apache-2.0 WITH LLVM-exception. See [LICENSE.TXT](LICENSE.TXT).
