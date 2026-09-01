# The image scav's Linux CI rows run in. It carries the compilers, their sanitizer
# and coverage runtimes, and the few programs the workflow itself shells out to.
# cmake, ninja, doctest and python are deliberately absent: those come from envy,
# and func.provisioning fails the build if a configured tree picked up any other.
#
# Rebuilt weekly from gcc:latest, so "the latest official GCC release" is whatever
# that tag resolves to on the Sunday the schedule fires.
FROM gcc:latest

# The major that tools/msan_libcxx.py builds its instrumented libc++ from. That
# libc++ and the clang consuming it have to stay within a major of each other.
ARG LLVM_VERSION=21

ENV DEBIAN_FRONTEND=noninteractive

# `. /etc/os-release` rather than a hardcoded suite: the base moves to the next
# Debian on its own schedule, and a stale suite here would install nothing while
# apt still exited 0.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg; \
    . /etc/os-release; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/llvm.gpg; \
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg]" \
         "https://apt.llvm.org/${VERSION_CODENAME}/" \
         "llvm-toolchain-${VERSION_CODENAME}-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list; \
    curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
      -o /usr/share/keyrings/githubcli.gpg; \
    echo "deb [signed-by=/usr/share/keyrings/githubcli.gpg]" \
         "https://cli.github.com/packages stable main" \
      > /etc/apt/sources.list.d/github-cli.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      binutils \
      build-essential \
      "clang-${LLVM_VERSION}" \
      file \
      gh \
      git \
      "libc++-${LLVM_VERSION}-dev" \
      "libc++abi-${LLVM_VERSION}-dev" \
      "libclang-rt-${LLVM_VERSION}-dev" \
      "lld-${LLVM_VERSION}" \
      "llvm-${LLVM_VERSION}" \
      unzip \
      xz-utils \
      zstd; \
    rm -rf /var/lib/apt/lists/*

# apt.llvm.org ships only versioned names in /usr/bin; the unversioned ones live
# here. The presets ask for `clang++`, and tools/coverage.py for `llvm-cov` and
# `llvm-profdata`, so this directory is what makes those resolvable.
ENV PATH="/usr/lib/llvm-${LLVM_VERSION}/bin:${PATH}"
