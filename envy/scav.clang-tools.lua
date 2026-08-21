-- @envy schema "1"
IDENTITY = "scav.clang-tools@r0"
EXPORTABLE = true

if envy.PLATFORM == "darwin" then
  DEPENDENCIES = { { product = "ninja" }, { product = "cmake" } }
end

OPTIONS = {
  version = { required = true },
  tools = {
    required = true,
    type = "list",
    validate = function(v)
      if #v == 0 then
        return "'tools' must be a non-empty array of tool names"
      end
      for i, tool in ipairs(v) do
        if type(tool) ~= "string" or tool == "" then
          return string.format("tool name at index %d must be a non-empty string", i)
        end
        if not tool:match("^[%w_%-%+]+$") then
          return string.format(
            "invalid tool name '%s': only letters, digits, '_', '-' and '+' are allowed",
            tool)
        end
      end
    end,
  },
}

FETCH = function(tmp_dir, opts)
  local base = "https://github.com/llvm/llvm-project/releases/download/llvmorg-" ..
      opts.version .. "/"

  if envy.PLATFORM == "darwin" then
    return base .. "llvm-project-" .. opts.version .. ".src.tar.xz"
  elseif envy.PLATFORM == "windows" then
    local arch = (envy.ARCH == "x86_64") and "x86_64" or "aarch64"
    return base ..
        "clang+llvm-" .. opts.version .. "-" .. arch .. "-pc-windows-msvc.tar.xz"
  elseif envy.PLATFORM == "linux" then
    local arch = (envy.ARCH == "x86_64") and "X64" or "ARM64"
    return base .. "LLVM-" .. opts.version .. "-Linux-" .. arch .. ".tar.xz"
  else
    error("unsupported platform: " .. envy.PLATFORM)
  end
end

-- The prebuilt archives carry an entire toolchain but INSTALL keeps only the requested
-- tools, so extract just those. darwin builds from source and needs the whole tree.
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts)
  local only
  if envy.PLATFORM ~= "darwin" then
    only = {}
    for i, tool in ipairs(opts.tools) do
      only[i] = "bin/" .. tool .. envy.EXE_EXT
    end
    -- clang-tidy compiles the translation unit, so it reads the builtin headers
    -- (`stdarg.h` and the rest) that clang-format never opens.
    only[#only + 1] = "lib/clang/*/include/**"
  end
  envy.extract_all(fetch_dir, stage_dir, { strip = 1, only = only })
end

if envy.PLATFORM == "darwin" then
  BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
    return envy.template([[
mkdir build
cd build
{{cmake}} -G Ninja ../llvm -DCMAKE_MAKE_PROGRAM={{ninja}} -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra"
{{ninja}} {{targets}}
]],
      {
        cmake = envy.product("cmake"),
        ninja = envy.product("ninja"),
        -- clang-resource-headers is what stages the builtin headers into the
        -- build tree; nothing else pulls it in when only a tool is asked for.
        targets = table.concat(opts.tools, " ") .. " clang-resource-headers"
      })
  end
end

-- The layout is the contract, not just the files: clang derives its resource
-- directory from argv[0] as ../lib/clang/<version>, so a flat install leaves
-- clang-tidy unable to open `stdarg.h` and every finding is a parse error.
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  local base = (envy.PLATFORM == "darwin")
      and envy.path.join(stage_dir, "build")
      or stage_dir
  for _, tool in ipairs(opts.tools) do
    envy.move(
      envy.path.join(base, "bin", tool .. envy.EXE_EXT),
      envy.path.join(install_dir, "bin", tool .. envy.EXE_EXT)
    )
  end
  envy.move(
    envy.path.join(base, "lib", "clang"),
    envy.path.join(install_dir, "lib", "clang")
  )
end

PRODUCTS = function(opts)
  local result = {}
  for _, tool in ipairs(opts.tools) do
    result[tool] = "bin/" .. tool .. envy.EXE_EXT
  end
  return result
end
