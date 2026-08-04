-- Local only because the first-party bundle records no 3.14 build yet. Delete this
-- file and go back to `envy.python@r0` as soon as it does.
--
-- @envy schema "1"

IDENTITY = "scav.python@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true }, release = { required = true } }

-- Linux x86_64 builds target the x86-64-v3 microarchitecture (AVX2).
local function platform_key()
  if envy.PLATFORM == "darwin" then
    return (envy.ARCH == "arm64") and "aarch64-apple-darwin" or "x86_64-apple-darwin"
  elseif envy.PLATFORM == "linux" then
    return (envy.ARCH == "x86_64") and "x86_64_v3-unknown-linux-gnu"
        or "aarch64-unknown-linux-gnu"
  end
  return "x86_64-pc-windows-msvc"
end

-- Hashes come from the SHA256SUMS asset in each release. Keyed by
-- "<version>+<release>": an interpreter version only means something alongside the
-- build that produced it.
local SHA256 = {
  ["3.14.6+20260728"] = {
    ["aarch64-apple-darwin"] =
    "bff75616d5f02111d2be7fe4786d4b574abcbc3f776c8852527d77a262bdb59b",
    ["x86_64-apple-darwin"] =
    "bcfd5e8983709e30176c1cd7e9c4b1cd2713931e5189073d30d9773ee05e65f3",
    ["aarch64-unknown-linux-gnu"] =
    "112319162e4b6305c5021c10591fd30cbe98f92c9359847ba58507e66e1e0fa9",
    ["x86_64_v3-unknown-linux-gnu"] =
    "008b89ed6d2b6861ef82c9e1f86acd604fa5d3e6b5702817563cd33b3da17748",
    ["x86_64-pc-windows-msvc"] =
    "75c0ad80e49bd241bd9828af4e3cd2b798677235466e1aa38f25a63a45f27fbb",
  },
}

FETCH = function(tmp_dir, opts)
  local pin, key = opts.version .. "+" .. opts.release, platform_key()
  local hash = SHA256[pin] and SHA256[pin][key]
  assert(hash, "no sha256 recorded for " .. pin .. " on " .. key)
  -- Windows builds have no LTO variant.
  local flavor = (envy.PLATFORM == "windows") and "pgo" or "pgo+lto"
  return {
    source = "https://github.com/astral-sh/python-build-standalone/releases/download/"
        .. opts.release .. "/cpython-" .. pin .. "-" .. key .. "-" .. flavor
        .. "-full.tar.zst",
    sha256 = hash,
  }
end

-- The archive nests everything under `python/`; `install/` is the prefix.
STAGE = { strip = 1 }

PRODUCTS = function(opts)
  local python = ((envy.PLATFORM == "windows") and "install/" or "install/bin/")
      .. "python" .. envy.EXE_EXT
  return {
    python = python,
    python3 = python,
    ["python" .. opts.version:match("^(%d+%.%d+)")] = python,
  }
end
