-- @envy schema "1"
IDENTITY = "scav.plantuml@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true } }

-- The GraalVM native builds, not the jar: no JRE to provision, and the image
-- cannot shell out to graphviz, so Smetana is the only layout engine present.
local ASSET_SLUG = {
  ["darwin-arm64"] = "macos-arm64",
  ["linux-x86_64"] = "linux-amd64",
  ["linux-arm64"] = "linux-arm64",
  ["windows-x86_64"] = "windows-amd64",
}

local SHA256 = {
  ["1.2026.7-macos-arm64"] =
  "f4e556a3d9fcf88730bed8b42018bb3e42dd475d8e988354ce1ab23b21c016f5",
  ["1.2026.7-linux-amd64"] =
  "048e13291e2303dba6be904cb00f0f6c81e96c11cb6092f8ba4b8f609f83c4a9",
  ["1.2026.7-linux-arm64"] =
  "c892800c61a6afc7854bf0a1f852a62b9b473ae76ae977a319535d56610a005f",
  ["1.2026.7-windows-amd64"] =
  "e92d8b4e509171d08e0586231d49a353cf87404e0d5e4972c5df53091bae8427",
}

FETCH = function(tmp_dir, opts)
  -- Upstream publishes no macos-amd64 native build, so an Intel mac has no
  -- baseline package rather than a silently different one.
  local slug = ASSET_SLUG[envy.PLATFORM_ARCH]
  assert(slug, "no native plantuml build for " .. envy.PLATFORM_ARCH)
  local key = opts.version .. "-" .. slug
  local sha = SHA256[key]
  assert(sha, "unpinned plantuml build: " .. key)

  return {
    source = "https://github.com/plantuml/plantuml/releases/download/v" ..
        opts.version .. "/native-plantuml-" .. slug .. "-" .. opts.version .. ".zip",
    sha256 = sha,
  }
end

-- The archive is flat, and on linux and windows it carries the AWT and freetype
-- shared libraries the binary loads from its own directory. Keeping the shipped
-- layout is what makes them findable.
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts)
  envy.extract_all(fetch_dir, stage_dir)
end

PRODUCTS = { plantuml = "plantuml" .. envy.EXE_EXT }
