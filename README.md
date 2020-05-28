# ExprCpp
Same as `std.Expr()`, but takes C++ functions instead of MaskTools RPN expressions.

## Example
```
user_func = '''
#include <cmath>

uint16_t func(uint8_t x, uint8_t y)
{
    if (std::abs(x - y) > 4) {
        return x * y;
    } else {
        return x * x;
    }
}
'''
clip = core.expr.expr_cpp((clip_a, clip_b), (user_func,))
```
yields the same result as
`clip = core.std.Expr((clip_a, clip_b), 'x y - abs 4 > x y * x x * ?')`
assuming both sources are 8-bit.

## Usage
### `expr_cpp()`
Currently ExprCpp mimics Expr user interface to the extent possible:
* `clips: Sequence[VideoNode]` — input clips.
* `code: Sequence[str]` — user code for each corresponding plane. As with Expr, empty string means copying, and no string at all means using the last one. See details in the next section.
* `format: Optional[VSFormat]` — output format. Defaults to the first input clip's format.
Debug options:
* `cxxflags: Optional[Sequence[str]]` — override optional flags supplied to compiler. Can be an empty sequence. Defaults are `("-O3", "-std=C++17", "-march=native")`.
* `dump_path: Optional[str]` — folder to place dumps to. Default to currend working directory.
* `dump_source: bool = false` — dump full source that goes to JIT.
* `dump_bitcode: bool = false` — dump LLVM IR bitcode outputted by Clang frontend. Use `llvm-dis` to get readable LLVM IR source.
* `dump_binary: bool = false` — dump native binary outputted by backend. Use `objdump -d` or `llvm-objdump -d` to get readable assembly.

### User code
Requirements:
1. Exactly one function (or overload set) in global namespace.
2. With signature compatible (in terms of C++) with input clips and output format.

Name doesn't matter.
Every piece of code is separated from others in runtime.

Restrictions:
1. Namespaces `exprcpp` and `expr` are reserved. Don't even mention them.
2. Half-precision floating-point (FP16) is not supported (yet).
3. Operator `new` is not supported (yet), as well as anyting that depends on it, including much of STL. But you can still get away with it if `new` will be optimized away.
4. Exceptions are currently not expected to work.

Things to keep in mind:
1. Unsigned types wrap on overflow and underflow. For your generic 8-bit source that uses `uint8_t` underneath it means that `uint8_t{-1} == 255` and `uint8_t{256} == 0`, for instance.
2. If output type in integer and you want your return values to be clamped (saturated), beware returning the type that exactly matches output type — return a wider type instead (note `uint16_t` in example in spite of 8-bit source).
3. Arithmetic in C++ has many other sharp corners, especially when it comes to floating-point. Stack Overflow and en.cppreference.com help a lot.
4. Unlike Expr, ExprCpp doesn't require input clips to have constant dimensions, but it still requires dimensions of input clips to match on every processed frame.

Usage tips:
1. If you need to split your processing into multiple functions, place additional ones into namespace. Common choices are `details`, `impl` or even unnamed, but not `exprcpp` — this one is reserved.
2. Use overloading or templates to generalize your functions for different outputs.

Tips to achieve best performance:
1. Avoid conversions. Take inputs using clip format's native type, mind your return type (also see (2) above).
2. Don't be clever. The better optimizer "understands" your code, the better output it can produce.
A bit of testing I've done using the example suggests that ExprCpp can be on par with Expr from performance standpoint.

## Building
### Prerequsites
* Compiler with C++17 support
* CMake 3.16.3+
* LLVM 10+ (`llvm-10-dev`)
* Clang 10+ (`libclang-cpp10-dev`)

### Linux
```
mkdir build
cd build
cmake ..
cmake --build .
```

### Windows
Windows distibution of LLVM doesn't expose C++ API nor as dynamic library neither as static one, so it's required to build it from source, and to let ExprCpp to statically link with it. ExprCpp supports building as an LLVM external project to achieve this:
1. Clone ExprCpp repo
2. Clone [LLVM monorepo](https://github.com/llvm/llvm-project)
3. Open Visual Studio Developer Console x64 and navigate to LLVM monorepo:
```
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang -DLLVM_EXTERNAL_PROJECTS=ExprCpp -DLLVM_EXTERNAL_EXPRCPP_SOURCE_DIR="path\to\exprcpp\root" -DCMAKE_CXX_STANDARD=17 -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON ../llvm
ninja exprcpp
```
specifying path to ExprCpp root directory.

Notes on CMake arguments:
1. It doesn't have to be Ninja, but I haven't tested other build systems.
2. Omit `-DCMAKE_BUILD_TYPE=Release` for debug build (or specify `Debug` explicitly).
3. Add `-DLLVM_ENABLE_LLD=ON` to enable `lld` linker. Highly recommend it for debug builds.
4. Add `-DLLVM_ENABLE_LTO=Full` to enable link-time optimization.
5. Add `-DLLVM_TARGETS_TO_BUILD=X86` to limit range of LLVM targets to just x86 (includes x86_64).

## Future Development
1. Allow keeping C++ code in a separate file, specifying just function names in Python code.
2. Migrate to C++20.
3. FP16 support.
4. STL support.

## Feedback
Thank you for getting this far. User feedback is what I'm always lacking, so feel free to join [this Telegram chat](https://t.me/vspreview_chat) to contact me on anything. Feedback could also make future plans become a reality sooner.