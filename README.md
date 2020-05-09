# ExprCpp
Same as `std.Expr()`, but takes arbitrary C++ functions instead of MaskTools RPN expressions.

## Example
```
user_func = '''
int func(int x, int y) {
    return x/4 + y/4;
}
'''
clip00 = core.endill.expr_cpp(clip00, clip00, user_func)
```

## Building Prerequsites for Linux
* Compiler with C++17 support
* CMake 3.16.3+
* LLVM 10 (`llvm-10-dev`)
* Clang 10 (`libclang-cpp10-dev`)
```
mkdir build
cd build
cmake ..
cmake --build .
```

## Building for Windows
Only as a part of LLVM build, because Windows distibution of LLVM doesn't expose C++ interface.
