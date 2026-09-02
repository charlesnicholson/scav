-- @envy schema "1"
IDENTITY = "scav.node@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true } }

local ASSET_SLUG = {
  ["darwin-arm64"] = "darwin-arm64",
  ["darwin-x86_64"] = "darwin-x64",
  ["linux-x86_64"] = "linux-x64",
  ["linux-arm64"] = "linux-arm64",
  ["windows-x86_64"] = "win-x64",
}

local SHA256 = {
  ["24.20.0-darwin-arm64"] =
  "b7bf7707070b950ba1ec5f1af3bb6de0f2b1962c5033973d94068ab021ef3014",
  ["24.20.0-darwin-x64"] =
  "26fc30891004603d094eed11de5efcd03bbd2efbc35c177fc72648d5d7a7701b",
  ["24.20.0-linux-x64"] =
  "2f2c0da162318f0de47665410c7c8c2ed3d36c8f3105de4bbc61176c70a7cbf2",
  ["24.20.0-linux-arm64"] =
  "5f4ddab610c1ab2016b3c227cebdbf6d9495161487e4739c7b90090595f465f7",
  ["24.20.0-win-x64"] =
  "6cac9ffbca8f6a47091e4b5c772e0606049c3871cb67d900c0cedde630e545ba",
}

-- The windows archive puts node.exe at the top of the versioned directory; every
-- other platform puts it under bin/. One strip leaves those two spellings.
local NODE_REL = (envy.PLATFORM == "windows") and ("node" .. envy.EXE_EXT) or "bin/node"

FETCH = function(tmp_dir, opts)
  local slug = ASSET_SLUG[envy.PLATFORM_ARCH]
  assert(slug, "no node build for " .. envy.PLATFORM_ARCH)
  local key = opts.version .. "-" .. slug
  local sha = SHA256[key]
  assert(sha, "unpinned node build: " .. key)

  local ext = (envy.PLATFORM == "windows") and ".zip" or ".tar.xz"
  return {
    source = "https://nodejs.org/dist/v" .. opts.version .. "/node-v" ..
        opts.version .. "-" .. slug .. ext,
    sha256 = sha,
  }
end

-- The interpreter alone. Nothing here runs npm, and the headers and bundled
-- modules are most of the 100 MB the archive unpacks to.
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts)
  envy.extract_all(fetch_dir, stage_dir, { strip = 1, only = { NODE_REL } })
end

PRODUCTS = { node = NODE_REL }
