-- @envy schema "1"
IDENTITY = "scav.elkjs@r0"
EXPORTABLE = true

-- The bundle is JavaScript: extracting it needs nothing, but it cannot run
-- without an interpreter, so the runtime travels with it. The manifest names
-- the version; this edge only says one is required.
DEPENDENCIES = { { product = "node" } }

OPTIONS = { version = { required = true } }

local SHA256 = {
  ["0.12.0"] = "c1d7719723e020b10724e3ccbc935696a2884f1e78d361cdd03766309e8e8e2a",
}

FETCH = function(tmp_dir, opts)
  local sha = SHA256[opts.version]
  assert(sha, "unpinned elkjs version: " .. opts.version)
  return {
    source = "https://registry.npmjs.org/elkjs/-/elkjs-" .. opts.version .. ".tgz",
    sha256 = sha,
  }
end

-- The bundled build pulls in no other packages, so the registry tarball is the
-- whole of elkjs and `npm install` buys nothing.
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts)
  envy.extract_all(fetch_dir, stage_dir,
    { strip = 1, only = { "lib/elk.bundled.js", "LICENSE.md" } })
end

PRODUCTS = { elk_bundle = "lib/elk.bundled.js" }
