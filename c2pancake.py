#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from string import Template
from typing import Optional


@dataclass
class FFIFunction:
    name: str


@dataclass
class ExportedFunction:
    name: str
    param_count: int = 0


@dataclass
class PancakeArtifact:
    pancake_source: str
    ffi_functions: list[FFIFunction] = field(default_factory=list)
    exported_functions: list[ExportedFunction] = field(default_factory=list)
    entry_function: Optional[str] = None
    max_base_slot: int = 0


_SCAFFOLD_TEMPLATE = Template("""\
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static char cml_memory[$total_memory];
extern void *cml_heap;
extern void *cml_stack;
extern void *cml_stackend;
extern void cml_main(void);
$extra_extern_decls
void cml_exit(int arg) {
    exit(arg);
}

void cml_err(int arg) {
    if (arg == 3) {
        fprintf(stderr,
            "Memory not ready for entry. "
            "You may have not run the init code yet, "
            "or be trying to enter during an FFI call.\\n");
    }
    cml_exit(arg);
}

void cml_clear() {}

static void init_pancake_mem(void) {
    unsigned long cml_heap_sz  = $heap_sz;
    unsigned long cml_stack_sz = $stack_sz;
    cml_heap     = cml_memory;
    cml_stack    = cml_heap + cml_heap_sz;
    cml_stackend = cml_stack + cml_stack_sz;
}

int main(void) {
    init_pancake_mem();
    cml_main();
$extra_calls    return 0;
}
""")

_FFI_STUB_TEMPLATE = Template("""\
void ffi$name(unsigned char *c, long clen, unsigned char *a, long alen) {
    /* TODO: implement $name */
}
""")

_MAKEFILE_TEMPLATE = """\
CAKE   ?= {cake_path}
TARGET ?= {target}
CC     ?= cc

CAKE_FLAGS = --target=$(TARGET) --pancake --main_return=true

PANCAKE_SRC = {name}.pancake
SCAFFOLD    = {name}.scaffold.c
FFI         = pancake_ffi.c
ASM         = {name}.S
BIN         = {name}.bin

.PHONY: all run clean

all: $(BIN)

$(ASM): $(PANCAKE_SRC)
\tcpp -P < $< | $(CAKE) $(CAKE_FLAGS) > $@

$(BIN): $(ASM) $(SCAFFOLD) $(FFI)
\t$(CC) -o $@ $^

run: $(BIN)
\t./$(BIN)

clean:
\trm -f $(ASM) $(BIN)
"""


class Transpiler:
    def __init__(self, binary: Path):
        self.binary = binary

    def transpile(self, source: Path) -> str:
        result = subprocess.run(
            [str(self.binary), str(source)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"c2pancake failed on {source}:\n{result.stderr}")
        return result.stdout


class FFIValidator:
    _FFI_FUNC_RE = re.compile(r"void\s+ffi(\w+)\s*\(")

    def parse_ffi_file(self, ffi_path: Path) -> set[str]:
        text = ffi_path.read_text()
        return {m.group(1) for m in self._FFI_FUNC_RE.finditer(text)}

    def validate(self, ffi_path: Path, required: list[FFIFunction]) -> None:
        if not required:
            return
        defined = self.parse_ffi_file(ffi_path)
        missing = [f.name for f in required if f.name not in defined]
        if missing:
            raise RuntimeError(
                f"Missing FFI implementations in {ffi_path.name}:\n"
                + "\n".join(
                    f"  - void ffi{n}(unsigned char *c, long clen, "
                    f"unsigned char *a, long alen)"
                    for n in missing
                )
            )


class PancakeAnalyser:
    _FFI_RE = re.compile(r"@(\w+)\s*\(")
    _EXPORT_RE = re.compile(r"export\s+fun\s+(\w+)\s*\(([^)]*)\)")
    _FUN_RE = re.compile(r"(?:export\s+)?fun\s+(\w+)\s*\(")
    _SLOT_RE = re.compile(r"@base slots \d+\.\.(\d+)")

    @staticmethod
    def _count_params(param_str: str) -> int:
        param_str = param_str.strip()
        if not param_str:
            return 0
        return len([p for p in param_str.split(",") if p.strip()])

    def analyse(self, pancake_source: str) -> PancakeArtifact:
        art = PancakeArtifact(pancake_source=pancake_source)

        ffi_names: set[str] = set()
        for m in self._FFI_RE.finditer(pancake_source):
            ffi_names.add(m.group(1))
        art.ffi_functions = [FFIFunction(n) for n in sorted(ffi_names)]

        for m in self._EXPORT_RE.finditer(pancake_source):
            name = m.group(1)
            param_count = self._count_params(m.group(2))
            art.exported_functions.append(ExportedFunction(name, param_count))

        all_funs = [m.group(1) for m in self._FUN_RE.finditer(pancake_source)]
        if "main" in all_funs:
            art.entry_function = "main"

        for m in self._SLOT_RE.finditer(pancake_source):
            slot = int(m.group(1))
            if slot >= art.max_base_slot:
                art.max_base_slot = slot + 1

        return art


class PancakeRewriter:
    def rewrite(self, source: str) -> str:
        return source


class ScaffoldGenerator:
    STACK_SIZE = 1024 * 50
    MIN_HEAP = 1024 * 50
    WORD_SIZE = 8

    def generate(self, art: PancakeArtifact) -> str:
        slots_bytes = art.max_base_slot * self.WORD_SIZE
        heap_sz = max(self.MIN_HEAP, slots_bytes + 1024 * 10)
        stack_sz = self.STACK_SIZE
        total_memory = heap_sz + stack_sz

        extra_exports = [e for e in art.exported_functions if e.name != "main"]

        extern_lines = ""
        for e in extra_exports:
            params = ", ".join(["long"] * e.param_count) if e.param_count else "void"
            extern_lines += f"extern long {e.name}({params});\n"

        call_lines = ""
        for e in extra_exports:
            args = ", ".join(["0"] * e.param_count) if e.param_count else ""
            call_lines += f"    {e.name}({args});\n"

        return _SCAFFOLD_TEMPLATE.substitute(
            total_memory=total_memory,
            heap_sz=heap_sz,
            stack_sz=stack_sz,
            extra_extern_decls=extern_lines,
            extra_calls=call_lines,
        )


class FFIGenerator:
    def generate(self, art: PancakeArtifact) -> str:
        header = "#include <stdio.h>\n\n"
        if not art.ffi_functions:
            return header + "/* No FFI functions required. */\n"
        stubs = "".join(
            _FFI_STUB_TEMPLATE.substitute(name=ffi.name) for ffi in art.ffi_functions
        )
        return header + stubs


class MakefileGenerator:
    def generate(
        self,
        program_name: str,
        cake_path: Path,
        target: str = "arm8",
    ) -> str:
        return _MAKEFILE_TEMPLATE.format(
            name=program_name,
            cake_path=cake_path,
            target=target,
        )


class Compiler:
    def __init__(self, cake_binary: Path, target: str = "arm8"):
        self.cake = cake_binary
        self.target = target

    def pancake_to_asm(self, pancake_path: Path, asm_path: Path) -> None:
        with open(pancake_path) as f:
            pancake_src = f.read()

        cpp_result = subprocess.run(
            ["cpp", "-P"],
            input=pancake_src,
            capture_output=True,
            text=True,
        )
        if cpp_result.returncode != 0:
            raise RuntimeError(f"cpp preprocessing failed:\n{cpp_result.stderr}")

        result = subprocess.run(
            [
                str(self.cake),
                f"--target={self.target}",
                "--pancake",
                "--main_return=true",
            ],
            input=cpp_result.stdout,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"cake compilation failed:\n{result.stderr}")
        asm_path.write_text(result.stdout)

    def link(
        self,
        asm_path: Path,
        scaffold_path: Path,
        ffi_path: Path,
        output_path: Path,
    ) -> None:
        result = subprocess.run(
            [
                "cc",
                "-o",
                str(output_path),
                str(asm_path),
                str(scaffold_path),
                str(ffi_path),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"Linking failed:\n{result.stderr}")


class Pipeline:
    def __init__(self, project_root: Path):
        self.root = project_root
        self.transpiler = Transpiler(project_root / "build" / "c2pancake")
        self.analyser = PancakeAnalyser()
        self.cake_path = project_root / "cake-arm8-64" / "cake"

    def run(
        self,
        source: Path,
        output_dir: Path,
        ffi_file: Optional[Path] = None,
        compile: bool = False,
        run_binary: bool = False,
    ) -> None:
        program_name = source.stem
        output_dir.mkdir(parents=True, exist_ok=True)

        raw_pancake = self.transpiler.transpile(source)

        art = self.analyser.analyse(raw_pancake)
        final_pancake = PancakeRewriter().rewrite(art.pancake_source)

        pancake_path = output_dir / f"{program_name}.pancake"
        scaffold_path = output_dir / f"{program_name}.scaffold.c"
        ffi_path = output_dir / "pancake_ffi.c"

        pancake_path.write_text(final_pancake)
        print(f"  Pancake source → {pancake_path}")

        scaffold_src = ScaffoldGenerator().generate(art)
        scaffold_path.write_text(scaffold_src)
        print(f"  C scaffolding  → {scaffold_path}")

        if ffi_file:
            import shutil

            shutil.copy2(ffi_file, ffi_path)
            print(f"  FFI (supplied)  → {ffi_path}")
            FFIValidator().validate(ffi_path, art.ffi_functions)
        else:
            ffi_src = FFIGenerator().generate(art)
            ffi_path.write_text(ffi_src)
            if art.ffi_functions:
                print(f"  FFI stubs      → {ffi_path}  (TODO: implement!)")
            else:
                print(f"  FFI stubs      → {ffi_path}")

        makefile_path = output_dir / "Makefile"
        makefile_src = MakefileGenerator().generate(
            program_name,
            self.cake_path,
        )
        makefile_path.write_text(makefile_src)
        print(f"  Makefile       → {makefile_path}")

        if compile:
            compiler = Compiler(self.cake_path)
            asm_path = output_dir / f"{program_name}.S"
            binary_path = output_dir / f"{program_name}.bin"

            print(f"\n  Compiling Pancake → assembly...")
            compiler.pancake_to_asm(pancake_path, asm_path)
            print(f"  Assembly       → {asm_path}")

            print(f"  Linking...")
            compiler.link(asm_path, scaffold_path, ffi_path, binary_path)
            print(f"  Binary         → {binary_path}")

            if run_binary:
                print(f"\n  Running {binary_path}...")
                subprocess.run([str(binary_path)])


def find_project_root() -> Path:
    here = Path(__file__).resolve().parent
    for candidate in [here] + list(here.parents):
        if (candidate / "build" / "c2pancake").exists():
            return candidate
    return here


def main() -> int:
    parser = argparse.ArgumentParser(
        description="C to Pancake full-pipeline tool",
    )
    parser.add_argument("source", type=Path, help="input C source file")
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        help="output directory (default: pipeline_output/)",
    )
    parser.add_argument(
        "--compile",
        action="store_true",
        help="compile through to a linked binary",
    )
    parser.add_argument(
        "--run",
        action="store_true",
        help="run the binary after compiling (implies --compile)",
    )
    parser.add_argument(
        "--ffi",
        type=Path,
        default=None,
        help="user-supplied pancake_ffi.c",
    )

    args = parser.parse_args()

    if args.run:
        args.compile = True

    if args.ffi and not args.ffi.exists():
        print(f"Error: {args.ffi} not found", file=sys.stderr)
        return 1

    if not args.source.exists():
        print(f"Error: {args.source} not found", file=sys.stderr)
        return 1

    root = find_project_root()
    if not (root / "build" / "c2pancake").exists():
        print(
            "Error: c2pancake binary not found. Run `cd build && make` first.",
            file=sys.stderr,
        )
        return 1

    output_dir = args.output_dir or root / "pipeline_output"

    print(f"c2pancake pipeline: {args.source.name}")

    try:
        pipeline = Pipeline(root)
        pipeline.run(
            source=args.source.resolve(),
            output_dir=output_dir,
            ffi_file=args.ffi.resolve() if args.ffi else None,
            compile=args.compile,
            run_binary=args.run,
        )
    except RuntimeError as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1

    print(f"\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
