#!/usr/bin/env python3
"""Check native C compilation and one-way dependencies of project components."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def check_layers() -> None:
    application_headers = {path.name for path in (ROOT / "main").rglob("*.h")}
    components = {path.name: path for path in (ROOT / "components").iterdir()
                  if path.is_dir() and (path / "CMakeLists.txt").is_file()}
    graph = {}
    for name, directory in components.items():
        for path in directory.rglob("*"):
            if path.suffix not in {".c", ".h"}:
                continue
            includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"\n]+)',
                                  path.read_text(encoding="utf-8"), re.M)
            for include in includes:
                if include in application_headers or "main/" in include.replace("\\", "/"):
                    raise ValueError(f"Component includes application header: {path.relative_to(ROOT)} -> {include}")
        cmake = (directory / "CMakeLists.txt").read_text(encoding="utf-8")
        groups = re.findall(r'\b(?:PRIV_)?REQUIRES\s+(.*?)(?=\b(?:PRIV_)?(?:REQUIRES|INCLUDE_DIRS)\b|\))',
                            cmake, re.S)
        dependencies = {token.strip('"') for group in groups for token in group.split()}
        if "main" in dependencies or re.search(r'(?:\.\./|PROJECT_DIR.*)[/\\]?main', cmake):
            raise ValueError(f"{name} depends on main")
        graph[name] = dependencies & components.keys()

    def visit(name: str, active: set[str], complete: set[str]) -> None:
        if name in active:
            raise ValueError(f"Component dependency cycle through {name}")
        if name in complete:
            return
        active.add(name)
        for dependency in graph[name]:
            visit(dependency, active, complete)
        active.remove(name)
        complete.add(name)

    complete = set()
    for name in graph:
        visit(name, set(), complete)


def check(build: Path | None = None) -> tuple[int, int]:
    check_layers()
    owned = [ROOT / "main", ROOT / "components"]
    cpp = [p for directory in owned for p in directory.rglob("*")
           if p.suffix.lower() in {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx"}]
    if cpp:
        raise ValueError("Project-owned C++ sources: " + ", ".join(str(p.relative_to(ROOT)) for p in cpp))
    sources = {p.resolve() for directory in owned for p in directory.rglob("*.c")}
    if not sources or not (ROOT / "main/main.c").is_file():
        raise ValueError("Missing native C application entry point")
    compiled = set()
    managed_cpp = 0
    if build is not None:
        for entry in json.loads((build / "compile_commands.json").read_text(encoding="utf-8")):
            source = Path(entry["file"]).resolve()
            command = entry.get("command", " ".join(entry.get("arguments", [])))
            if source in sources:
                if ROOT / "components" in source.parents:
                    include_paths = re.findall(r'(?:^|\s)-I\s*(?:"([^"]+)"|(\S+))', command)
                    for quoted, plain in include_paths:
                        include = Path(quoted or plain).resolve()
                        if include == ROOT / "main" or ROOT / "main" in include.parents:
                            raise ValueError(f"Component compile exposes main headers: {source.name}")
                standards = re.findall(r"-std=([^\s]+)", command)
                if not standards or standards[-1] not in {"c17", "gnu17"}:
                    raise ValueError(f"{source.name} is not compiled as C17")
                if re.search(r"(?:g\+\+|clang\+\+)(?:\.exe)?\s", command):
                    raise ValueError(f"{source.name} uses a C++ compiler")
                compiled.add(source)
            if ROOT / "managed_components" in source.parents and source.suffix in {".cpp", ".cc", ".cxx"}:
                managed_cpp += 1
        missing = sources - compiled
        if missing:
            raise ValueError("C sources omitted from the build: " + ", ".join(p.name for p in missing))
    return len(sources), managed_cpp


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, help="Also inspect an IDF build's compile_commands.json")
    args = parser.parse_args()
    try:
        count, dependencies = check(args.build)
        detail = f"; {dependencies} managed C++ compilation entries allowed" if args.build else ""
        print(f"Verified {count} project C sources and component dependency boundaries{detail}.")
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + "\n")
