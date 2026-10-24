# Linux Kernel Data Structures & Algorithms: companion code

Teaching implementations and tests for the book *Linux Kernel Data Structures & Algorithms* by Jinbo Wang. Each directory `chNN/` holds the code for Chapter NN. The book's code listings are taken from these files.

The code is a set of small, self-contained userspace models of kernel structures, written to make each algorithm easy to read and test. It is not kernel source. Facts about the kernel in the book refer to Linux v6.18, with differences up to v7.3 described at the end of each chapter.

## Building and testing

You need Linux, GCC, and `make`. Clang also works if its runtime library is installed (on Debian and Ubuntu, `libclang-rt-dev`), because the sanitizer checks for 128-bit multiplication in chapters 12, 13 and 18 need it: `make check CC="clang -rtlib=compiler-rt"`. Some programs use Linux-only interfaces such as `memfd_create()`, so the code does not build on macOS.

```sh
cd ch02
make check        # build with -Wall -Wextra, run the randomized tests under
                  # AddressSanitizer/UndefinedBehaviorSanitizer, then the demos
make clean
```

To check every chapter:

```sh
for d in ch*/; do make -C "$d" check || break; done
```

Chapter 21 has no code. `ch08/maple_alloc_demo.c` is an exercise that builds against the kernel's own maple tree in `tools/testing/radix-tree` of a v6.18 source tree; its header comment gives the commands. `make check` does not build it.

## License

The code is under the MIT License (see `LICENSE`), except `ch08/maple_alloc_demo.c`, which links with kernel code and is under GPL-2.0-or-later (see `LICENSES/GPL-2.0`). Every file carries an `SPDX-License-Identifier` line.
