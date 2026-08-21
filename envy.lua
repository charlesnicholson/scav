-- scav's toolchain and package manifest, and envy's root marker. Everything scav
-- generates lives under out/, so the package cache does too.

-- @envy schema "1"
-- @envy version "0.1.9"
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

-- The lint gates, behind SCAV_LINT because nothing compiles with them: an
-- ordinary build, and every CI row but the lint one, never downloads a
-- toolchain it would not use. Set the variable to run the gates anywhere,
-- which on darwin means compiling clang once. Pinned rather than installed by
-- the job, so the version judging a diff is the version in this file.
if os.getenv("SCAV_LINT") then
  envy.extend(PACKAGES, {
    { spec = "scav.clang-tools@r0",
      source = envy.abspath("envy") .. "/scav.clang-tools.lua",
      options = { version = "22.1.8", tools = { "clang-format", "clang-tidy" } } },
  })
end
