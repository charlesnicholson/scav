-- clang-format and clang-tidy, pinned: their output moves between releases, so an
-- unpinned formatter turns a review into a diff about whitespace nobody wrote.

-- LLVM publishes no tools-only archive, so this downloads a whole release to
-- install two binaries. The cached package is small; the download is not.

-- @envy schema "1"

IDENTITY = "scav.clang-tools@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true, choices = { "21.1.8" } } }

-- Upstream's artifact naming is not consistent between platforms, and macOS
-- x86_64 is absent entirely, so that host gets a diagnostic rather than a 404.
local function platform_key()
  if envy.PLATFORM == "darwin" then
    assert(envy.ARCH == "arm64",
      "LLVM publishes no macOS x86_64 release archive; clang-format and " ..
      "clang-tidy are unavailable on this host")
    return "macOS-ARM64"
  elseif envy.PLATFORM == "linux" then
    return (envy.ARCH == "x86_64") and "Linux-X64" or "Linux-ARM64"
  elseif envy.PLATFORM == "windows" then
    return (envy.ARCH == "x86_64") and "x86_64-pc-windows-msvc"
        or "aarch64-pc-windows-msvc"
  end
  error("unsupported platform: " .. envy.PLATFORM)
end

local function archive_name(version, key)
  if envy.PLATFORM == "windows" then
    return "clang+llvm-" .. version .. "-" .. key .. ".tar.xz"
  end
  return "LLVM-" .. version .. "-" .. key .. ".tar.xz"
end

-- Hashes come from the release assets' own `digest` field on the GitHub releases
-- API, not recomputed locally.
local SHA256 = {
  ["21.1.8"] = {
    ["macOS-ARM64"] =
    "b95bdd32a33a81ee4d40363aaeb26728a26783fcef26a4d80f65457433ea4669",
    ["Linux-X64"] =
    "b3b7f2801d15d50736acea3c73982994d025b01c2f035b91ae3b49d1b575732b",
    ["Linux-ARM64"] =
    "65ce0b329514e5643407db2d02a5bd34bf33d159055dafa82825c8385bd01993",
    ["x86_64-pc-windows-msvc"] =
    "749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47",
    ["aarch64-pc-windows-msvc"] =
    "f214b1226d8de005b5f691dd29d9dfea2b49e22d0de445429916173dbb626f7f",
  },
}

FETCH = function(tmp_dir, opts)
  local key = platform_key()
  local name = archive_name(opts.version, key)
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
