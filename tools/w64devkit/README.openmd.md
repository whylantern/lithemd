# Bundled MinGW-w64 toolchain

This directory is a trimmed copy of [w64devkit 2.11.0](https://github.com/skeeto/w64devkit/releases)
so `build.bat` / `build.ps1` can compile openmd without installing a compiler.

Kept:

- `bin/g++.exe`, `gcc.exe`, `cpp.exe`, assembler/linker, `windres.exe`
- x86_64 MinGW-w64 headers and import libraries
- GCC 16.2.0 runtime libraries (`libstdc++`, `libgcc`, etc.)
- `libexec/gcc/.../cc1.exe`, `cc1plus.exe`, `collect2.exe`

Removed to reduce size: 32-bit libraries, documentation, CMake, gdb, make and
unrelated tools. The original w64devkit license files are included
(`COPYING.MinGW-w64-runtime.txt`, `README.md`, `VERSION.txt`).

The toolchain is only used at build time. The produced `dist\openmd.exe` does
not require MinGW, GCC or any other runtime on the target machine.
