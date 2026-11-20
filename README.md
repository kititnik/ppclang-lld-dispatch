# ppclang: dispatch tables built by the linker

[ppclang](https://gitverse.ru/kpdev/llvm-project) is a Clang-based compiler for
[procedural-parametric C](https://softcraft.ru/ppp/), a C extension with open generic types and multimethods.
A generic type gets new specializations, and a generic function gets new handlers,
from any file in the program. So no single translation unit knows the whole
dispatch table.

ppclang used to assemble those tables at runtime: every handler came with a
registration constructor that ran before `main`. This fork moves the job to
the linker. The compiler emits small metadata records, and LLD builds the
finished tables at link time. As a result, nothing runs at startup.

## Example

```c
// figure.h: a generic type and a generic function
typedef struct Figure { int id; } <> Figure;
void Print<Figure* f>(FILE* out);

// rect.c: a specialization and its handler, added in a separate file
typedef struct Rectangle { int x, y; } Rectangle;
Figure + <rect: Rectangle;>;

void Print<Figure.rect* f>(FILE* out) {
    fprintf(out, "rect %d x %d\n", f->@x, f->@y);
}

// main.c
Print<fig>(stdout);   // one indexed jump through a table that LLD filled in
```

## How it works

For every generic type and function, the compiler emits records into three kinds
of sections:

- `pp_specs_<Gen>`: one record per specialization (its name and its `create_spec`);
- `pp_impl_<Fn>`: one record per handler (the handler address plus the specialization on each axis);
- `pp_desc_<Fn>`: one record per generic function (the default handler and its axes).

LLD merges same-named sections across all object files, as it always does. A
specialization's tag is simply its position in the merged `pp_specs_` section,
so the object tag, the table index and the `get_spec_ptr` index all come from
the same place and cannot disagree. LLD exposes each tag as an absolute symbol,
reads the `pp_impl_` records through their relocations, and fills in
`pp_table_<Fn>`. It reports duplicate handlers, and it warns about generic
functions that have no handlers.

## Results

Six test programs were measured before and after the change (details in
[docs/benchmark.md](docs/benchmark.md)):

| | before | after |
|---|---|---|
| Time to `main`, 4096-handler program | 15.2 ms | 6.0 ms (−61%) |
| Constructors run before `main` | 44 – 12 615 | 0 |
| Heap allocated before `main` | 1 – 786 KB | 0 |
| Stripped binary size | | −45…−53% |
| Compile time | | −8…−21% |
| Extra link time spent on tables | | +1…3% |
| Cost of a call | | unchanged |

On the clang side the change removes 734 lines of code. The table builder is a
single new file in LLD of about 560 lines.

## Limitations

- Only ELF on x86-64, and only `-no-pie`.
- Shared libraries are not supported. Static archives need `--whole-archive`.
- Tags depend on the order of object files on the command line.
- A sparse table with high arity is stored in the file in full: `Π Nᵢ × 8` bytes.

## Where the code is

- **All changes in this fork:** [`pp-extension-v21.1.8...pp-linker-tables`](https://github.com/kititnik/ppclang-lld-dispatch/compare/pp-extension-v21.1.8...pp-linker-tables)
- **Table builder:** [`lld/ELF/PPTable.cpp`](lld/ELF/PPTable.cpp)
- **Compiler side:** `clang/lib/CodeGen`, `clang/lib/Parse`

## Building

Build it like a normal LLVM build, with `clang` and `lld` enabled:

```sh
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=X86
ninja -C build clang lld
build/bin/clang -no-pie -fuse-ld=lld --ld-path=build/bin/ld.lld *.c
```

## Credits

The procedural-parametric approach is by A. I. Legalov
([overview](https://softcraft.ru/ppp/),
[paper](https://www.mais-journal.ru/jour/article/view/1766), both in Russian).

ppclang, the base of this fork, is by Pavel Kosov
([gitverse.ru/kpdev/llvm-project](https://gitverse.ru/kpdev/llvm-project)).

The link-time tables in this fork are my work.

LLVM is distributed under the Apache License v2.0 with LLVM Exceptions, see
[LICENSE.TXT](LICENSE.TXT).
