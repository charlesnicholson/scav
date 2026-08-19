#!/usr/bin/env python3
"""Build a libc++ instrumented with MemorySanitizer, without which MSan reports
false positives that look exactly like real findings until someone switches it off.

    $(./bin/envy product python3) tools/msan_libcxx.py            # build, print the prefix
    $(./bin/envy product python3) tools/msan_libcxx.py --prefix   # print the prefix, build nothing

The printed prefix is what -DSCAV_MSAN_LIBCXX_DIR wants:

    ./build.sh --sanitizer msan          # builds it and passes the prefix for you
"""

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENVY = REPO_ROOT / ("bin/envy.bat" if platform.system() == "Windows" else "bin/envy")
WORK_DIR = REPO_ROOT / "out/msan-libcxx"
PREFIX = WORK_DIR / "prefix"
MARKER = PREFIX / "include/c++/v1/vector"

# Pinned and hash-verified, being a compilation input. The major tracks the
# clang that builds and consumes it; three majors apart is a build failure.
VERSION = "21.1.8"
TARBALL = f"llvm-project-{VERSION}.src.tar.xz"
URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{TARBALL}"
SHA256 = "4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142"

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


def run(*cmd: str | Path) -> None:
    argv = [str(c) for c in cmd]
    print(f"+ {' '.join(argv)}", file=sys.stderr, flush=True)
    # Captured and replayed rather than inherited: a build this long interleaves
    # badly in a CI log, and a failure has to arrive next to its command.
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    if result.returncode:
        print(result.stdout, file=sys.stderr, flush=True)
        raise SystemExit(f"{argv[0]} failed with exit status {result.returncode}")


def envy(*args: str | Path) -> str:
    """Downloading, hashing and unpacking go through envy's pinned libcurl, TLS and
    libarchive rather than whatever the interpreter and the machine happen to have."""
    return subprocess.run([str(ENVY), *(str(a) for a in args)], cwd=REPO_ROOT,
                          check=True, stdout=subprocess.PIPE, text=True).stdout.strip()


def sha256_of(path: Path) -> str:
    return envy("hash", path).split()[0]


def download() -> Path:
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    archive = WORK_DIR / TARBALL
    if archive.is_file() and sha256_of(archive) == SHA256:
        return archive

    envy("fetch", URL, archive)
    if (actual := sha256_of(archive)) != SHA256:
        archive.unlink()
        raise SystemExit(f"{TARBALL} sha256 {actual}, expected {SHA256}")
    return archive


def extract(archive: Path) -> Path:
    runtimes = WORK_DIR / f"llvm-project-{VERSION}.src/runtimes"
    if not (runtimes / "CMakeLists.txt").is_file():
        envy("extract", archive, WORK_DIR)
    if not (runtimes / "CMakeLists.txt").is_file():
        raise SystemExit(f"{TARBALL} did not contain {runtimes.parent.name}/runtimes")
    return runtimes


def build(runtimes: Path) -> None:
    cmake, build_dir = envy("product", "cmake"), WORK_DIR / "build"
    clang, clangxx = shutil.which("clang"), shutil.which("clang++")
    if not clang or not clangxx:
        raise SystemExit(
            "clang and clang++ must be on PATH: MSan's instrumented libc++ has to "
            "be built by the same compiler that will consume it."
        )
    run(cmake, "-S", runtimes, "-B", build_dir, "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={envy('product', 'ninja')}",
        f"-DCMAKE_C_COMPILER={clang}", f"-DCMAKE_CXX_COMPILER={clangxx}",
        # LLVM's runtimes build wants a Python of its own, and a bare runner has
        # none. Hand it the provisioned one rather than installing a second.
        f"-DPython3_EXECUTABLE={envy('product', 'python3')}",
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
