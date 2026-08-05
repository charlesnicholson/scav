-- clang-format and clang-tidy, pinned: their output moves between releases, so an
-- unpinned formatter turns a review into a diff about whitespace nobody wrote.

-- Linux only, and CI is the only place that wants it. LLVM publishes no tools-only
-- archive, so this downloads a whole release to install two binaries -- too big to
-- put on every developer's machine, and its macOS builds are not published for
-- every release anyway.

-- @envy schema "1"

IDENTITY = "scav.clang-tools@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true, choices = { "21.1.8" } } }

-- LLVM's Linux archives are published for every release. Its macOS ones are not --
-- 21.1.0 has none at all, x86_64 stopped at 19.1.7, and the naming has changed
-- twice -- which is why formatting and linting are a Linux job.
local function platform_key()
  assert(envy.PLATFORM == "linux",
    "clang-format and clang-tidy are provisioned on Linux only; formatting and " ..
    "linting are gated in CI rather than on every developer's machine")
  return (envy.ARCH == "x86_64") and "Linux-X64" or "Linux-ARM64"
end

-- Hashes come from the release assets' own `digest` field on the GitHub releases
-- API, not recomputed locally.
local SHA256 = {
  ["21.1.8"] = {
    ["Linux-X64"] =
    "b3b7f2801d15d50736acea3c73982994d025b01c2f035b91ae3b49d1b575732b",
    ["Linux-ARM64"] =
    "65ce0b329514e5643407db2d02a5bd34bf33d159055dafa82825c8385bd01993",
  },
}

FETCH = function(tmp_dir, opts)
  local key = platform_key()
  local name = "LLVM-" .. opts.version .. "-" .. key .. ".tar.xz"
  local hash = SHA256[opts.version] and SHA256[opts.version][key]
  assert(hash, "no sha256 recorded for " .. opts.version .. " on " .. key)
  return {
    source = "https://github.com/llvm/llvm-project/releases/download/llvmorg-" ..
        opts.version .. "/" .. name,
    sha256 = hash,
  }
end

STAGE = { strip = 1 }

local TOOLS = { "clang-format", "clang-tidy" }

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  for _, tool in ipairs(TOOLS) do
    envy.copy(envy.path.join(stage_dir, "bin", tool .. envy.EXE_EXT),
      envy.path.join(install_dir, "bin", tool .. envy.EXE_EXT))
  end

  -- clang-tidy resolves clang's builtin headers relative to its own executable, so
  -- the two binaries alone are not a working package.
  local major = opts.version:match("^(%d+)")
  local resource = envy.path.join("lib", "clang", major, "include")
  envy.copy(envy.path.join(stage_dir, resource),
    envy.path.join(install_dir, resource))
end

PRODUCTS = {
  ["clang-format"] = envy.path.join("bin", "clang-format" .. envy.EXE_EXT),
  ["clang-tidy"] = envy.path.join("bin", "clang-tidy" .. envy.EXE_EXT),
}
