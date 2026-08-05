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
    identity = "envy.package-specs@r2",
    source = "https://github.com/envy-package-manager/package-specs.git",
    ref = "4abc43074b424400f7d518ef925f8ab8d4624060",
  },
}

PACKAGES = {
  { spec = "envy.cmake@r0", bundle = "envy", options = { version = "4.4.0" } },

  { spec = "envy.ninja@r0", bundle = "envy", options = { version = "1.13.2" } },

  { spec = "envy.doctest-cpp@r0", bundle = "envy", options = { version = "2.5.3" } },

  { spec = "envy.python@r1", bundle = "envy",
    options = { version = "3.14.6", release = "20260623",
                provide_python = true, provide_python3 = true } },
}
