// Test reflection extension: ^^type-id with non-builtin types
// RUN: ~/llvm-build/bin/clang-23 -std=c++26 -freflection -fsyntax-only %s 2>&1

struct MyStruct {
    int x;
    double y;
};

namespace MyNS {
    int value = 42;
}

// Test: reflect a class type
auto r1 = ^^MyStruct;

// Test: reflect a namespace-qualified type
auto r2 = ^^MyNS;

// Test: reflect the global namespace
auto r3 = ^^::;

// Test: reflect a fundamental type
auto r4 = ^^int;

// Test: reflect a pointer type
auto r5 = ^^int*;
