# Linux Kernel Data Structures & Algorithms: companion code

Teaching implementations and tests for the book *Linux Kernel Data Structures & Algorithms* by Jinbo Wang. Each directory `code/chNN/` holds the code for Chapter NN, matching the paths used in the book. The book's code listings are taken from these files.

The code is a set of small, self-contained userspace models of kernel structures, written to make each algorithm easy to read and test. It is not kernel source. Facts about the kernel in the book refer to Linux v6.18, with differences up to v7.3 described at the end of each chapter.

## Building and testing

You need Linux, GCC, and `make`. Clang also works. With some Clang versions (for example Clang 19 on Debian 13, arm64), the sanitizer builds of chapters 12 and 18 fail to link with plain `CC=clang` because their 128-bit multiplications need a helper that GCC's runtime does not provide; install Clang's runtime (`libclang-rt-dev` on Debian and Ubuntu) and use `make check CC="clang -rtlib=compiler-rt"`. Some programs use Linux-only interfaces such as `memfd_create()`, so the code does not build on macOS.

```sh
cd code/ch02
make check        # build with -Wall -Wextra, run the randomized tests under
                  # AddressSanitizer/UndefinedBehaviorSanitizer, then the demos
make clean
```

To check every chapter:

```sh
for d in code/ch*/; do make -C "$d" check || break; done
```

Chapter 21 has no code. `code/ch08/maple_alloc_demo.c` is an exercise that builds against the kernel's own maple tree in `tools/testing/radix-tree` of a v6.18 source tree; its header comment gives the commands. `make check` does not build it.

## License

The code is under the MIT License (see `LICENSE`), except `code/ch08/maple_alloc_demo.c`, which links with kernel code and is under GPL-2.0-or-later (see `LICENSES/GPL-2.0`). Every file carries an `SPDX-License-Identifier` line.

## Errata and feedback

Corrections to the book are listed in [ERRATA.md](ERRATA.md). To report an error in the book or a bug in the code, open an issue on this repository; please give the chapter and section, or the file and line.
