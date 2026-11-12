"""Small dependency-free CLI entry point for the CMake toolchain matrix."""
import argparse
from .commands.core import execute

def main():
    parser = argparse.ArgumentParser(prog="zerolog")
    parser.add_argument("--version", action="version", version="zerolog 0.1.0")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("setup", "build", "test", "bench", "format", "tidy", "verify"):
        cmd = sub.add_parser(name)
        cmd.add_argument("--preset", default="clang-glibc-release")
        cmd.add_argument("--build-dir")
        cmd.add_argument("--jobs", type=int, default=4)
        if name == "format": cmd.add_argument("--check", action="store_true")
        if name == "verify":
            cmd.add_argument("--tier", type=int, choices=(1,2), default=1)
            cmd.add_argument("--object", action="append", default=[])
            cmd.add_argument("--gates")
            cmd.add_argument("--ir")
    args = parser.parse_args()
    try:
        execute(args)
    except (RuntimeError, OSError) as error:
        parser.exit(1, f"zerolog: {error}\n")
