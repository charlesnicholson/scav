-- scav's toolchain and package manifest, and envy's root marker. Everything scav
-- generates lives under out/, so the package cache does too.

-- @envy schema "1"
-- @envy version "0.1.2"
-- @envy bin "bin"
-- @envy cache-posix "out/.envy"
-- @envy cache-win "out\.envy"
-- @envy deploy "true"

BUNDLES = {
  ["envy"] = {
    identity = "envy.package-specs@r1",
    source = "https://github.com/envy-package-manager/package-specs.git",
    ref = "9bdb0a11cefa3e83418cff37dc68ea755c07a237",
  },
}

PACKAGES = {
  { spec = "envy.cmake@r0", bundle = "envy", options = { version = "4.4.0" } },

  { spec = "envy.ninja@r0", bundle = "envy", options = { version = "1.13.2" } },

  { spec = "envy.doctest-cpp@r0", bundle = "envy", options = { version = "2.5.3" } },

  { spec = "scav.python@r0", source = "./envy/scav.python.lua",
    options = { version = "3.14.6", release = "20260728" } },
}

-- Gated: LLVM ships no tools-only archive, so two binaries cost a whole release
-- download. Opt in with `SCAV_CLANG_TOOLS=1 ./bin/envy sync`.
if os.getenv("SCAV_CLANG_TOOLS") then
  envy.extend(PACKAGES, {
    { spec = "scav.clang-tools@r0", source = "./envy/scav.clang-tools.lua",
      options = { version = "21.1.8" } },
  })
end
