#!/usr/bin/env python3
"""Build a libc++ instrumented with MemorySanitizer, without which MSan reports
false positives that look exactly like real findings until someone switches it off.

    python3 tools/msan_libcxx.py            # build, print the prefix
    python3 tools/msan_libcxx.py --prefix   # print the prefix, build nothing

The printed prefix is what -DSCAV_MSAN_LIBCXX_DIR wants:

    ./build.sh --sanitizer msan -- -DSCAV_MSAN_LIBCXX_DIR=$(python3 tools/msan_libcxx.py)
"""

import argparse
import hashlib
import lzma
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENVY = REPO_ROOT / ("bin/envy.bat" if platform.system() == "Windows" else "bin/envy")
WORK_DIR = REPO_ROOT / "out/msan-libcxx"
PREFIX = WORK_DIR / "prefix"
MARKER = PREFIX / "include/c++/v1/vector"

# Pinned and hash-verified: this is a compilation input, so an unpinned one makes
# the MSan row depend on whatever upstream published that morning.
VERSION: str = "21.1.8"
TARBALL = f"llvm-project-{VERSION}.src.tar.xz"
URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{TARBALL}"
SHA256: str = "4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142"

CMAKE_FLAGS: list[str] = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind",
    # Origin tracking turns "some byte was uninitialized" into a stack naming where
    # it came from.
    "-DLLVM_USE_SANITIZER=MemoryWithOrigins",
    "-DLIBCXX_ENABLE_SHARED=ON",
    "-DLIBCXXABI_ENABLE_SHARED=ON",
    "-DLIBCXX_CXX_ABI=libcxxabi",
    # Otherwise these link in uninstrumented -- exactly the false-positive source
    # this exercise exists to remove.
    "-DLIBCXX_USE_COMPILER_RT=ON",
    "-DLIBCXXABI_USE_LLVM_UNWINDER=ON",
    "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
    "-DLIBCXX_INCLUDE_TESTS=OFF",
    "-DLIBCXXABI_INCLUDE_TESTS=OFF",
]


def envy_product(name: str) -> str:
    """Ask envy, so this uses the same cmake and ninja as the build."""
    return subprocess.run([str(ENVY), "product", name], cwd=REPO_ROOT, check=True,
                          stdout=subprocess.PIPE, text=True).stdout.strip()


def run(*cmd: str | Path) -> None:
    argv = [str(c) for c in cmd]
    print(f"+ {' '.join(argv)}", file=sys.stderr, flush=True)
    subprocess.run(argv, check=True)


def sha256_of(path: Path) -> str:
    with open(path, "rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def download() -> Path:
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    if (archive := WORK_DIR / TARBALL).exists() and sha256_of(archive) == SHA256:
        return archive

    print(f"downloading {URL}", file=sys.stderr, flush=True)
    with urllib.request.urlopen(URL) as response, open(archive, "wb") as out:
        shutil.copyfileobj(response, out)

    if (actual := sha256_of(archive)) != SHA256:
        archive.unlink()
        raise SystemExit(f"{TARBALL} sha256 {actual}, expected {SHA256}")
    return archive


def extract(archive: Path) -> Path:
    source = WORK_DIR / f"llvm-project-{VERSION}.src"
    if (runtimes := source / "runtimes").joinpath("CMakeLists.txt").is_file():
        return runtimes

    print(f"extracting {archive.name}", file=sys.stderr, flush=True)
    with lzma.open(archive) as decompressed, tarfile.open(fileobj=decompressed,
                                                          mode="r|") as tar:
        # Refuses absolute paths and escaping symlinks. The archive is hash-verified
        # already, but the check is free.
        tar.extractall(path=WORK_DIR, filter="data")
    if not (runtimes / "CMakeLists.txt").is_file():
        raise SystemExit(f"{archive.name} did not contain {source.name}/runtimes")
    return runtimes


def build(runtimes: Path) -> None:
    cmake, build_dir = envy_product("cmake"), WORK_DIR / "build"
    if not (clang := shutil.which("clang")) or not (clangxx := shutil.which("clang++")):
        raise SystemExit(
            "clang and clang++ must be on PATH: MSan's instrumented libc++ has to "
            "be built by the same compiler that will consume it."
        )
    run(cmake, "-S", runtimes, "-B", build_dir, "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={envy_product('ninja')}",
        f"-DCMAKE_C_COMPILER={clang}", f"-DCMAKE_CXX_COMPILER={clangxx}",
        f"-DCMAKE_INSTALL_PREFIX={PREFIX}", *CMAKE_FLAGS)
    run(cmake, "--build", build_dir)
    run(cmake, "--install", build_dir)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", action="store_true",
                        help="print the prefix and build nothing")

    if not parser.parse_args().prefix and not MARKER.is_file():
        if platform.system() != "Linux":
            raise SystemExit(
                f"MSan is Linux/clang only; this host is {platform.system()}. There "
                "is no instrumented libc++ to build anywhere else, which is why the "
                "matrix has exactly one MSan row."
            )
        build(extract(download()))
        if not MARKER.is_file():
            raise SystemExit(f"install finished but {PREFIX} has no libc++ headers")

    print(PREFIX)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
