#!/usr/bin/env python3
"""With SCAV_REQUIRE_ENVY=1, every tool in the build config came from envy. Always:
the envy cache sits under out/."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

# The build-config keys naming a tool envy is expected to have provisioned.
PROVISIONED: tuple[str, ...] = (
    "cmake", "make_program", "doctest_include_dir", "python",
)


def envy(repo_root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    launcher = "bin/envy.bat" if os.name == "nt" else "bin/envy"
    return subprocess.run([str(repo_root / launcher), *args], cwd=repo_root,
                          capture_output=True, text=True, check=False)


class TestProvisioning(unittest.TestCase):
    cfg: scavtest.Config
    packages: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        resolved = envy(cls.cfg.repo_root, "cache", "--root")
        assert resolved.returncode == 0, resolved.stderr
        cls.packages = (Path(resolved.stdout.strip()) / "packages").resolve()

    def setUp(self) -> None:
        if os.environ.get("SCAV_REQUIRE_ENVY") != "1":
            self.skipTest(
                "SCAV_REQUIRE_ENVY is not 1. envy pins the toolchain for scav's "
                "own CI and is deliberately not a build prerequisite, so a build "
                "with your own cmake is supported and this check is CI's."
            )

    def test_every_tool_came_from_an_envy_package(self) -> None:
        for key in PROVISIONED:
            with self.subTest(tool=key):
                self.assertTrue(value := self.cfg[key], f"{key} is empty")
                path = Path(value).resolve()
                self.assertTrue(path.is_relative_to(self.packages),
                                f"{key} resolved to {path}, outside {self.packages}")


class TestSandbox(unittest.TestCase):
    """The envy cache lives under out/, checked whether or not envy is required."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        cls.manifest = (cls.cfg.repo_root / "envy.lua").read_text(encoding="utf-8")

    def test_the_manifest_asks_for_a_cache_under_out(self) -> None:
        self.assertIn('-- @envy cache-local "out/.envy"', self.manifest)

    def test_the_manifest_does_not_use_the_removed_directives(self) -> None:
        for gone in ("cache-posix", "cache-win"):
            self.assertNotIn(gone, self.manifest)

    def test_naming_the_tree_is_what_selects_local_mode(self) -> None:
        """`cache-local` alone selects local mode; the manifest sets no `cache-mode`."""
        self.assertNotIn("cache-mode", self.manifest)

    def test_the_resolved_root_is_under_out_on_a_clean_checkout(self) -> None:
        """`envy cache --root` resolves to out/.envy on a clean checkout."""
        state = self.cfg.repo_root
        if os.environ.get("ENVY_CACHE_ROOT") or any(
                (state / m).exists()
                for m in (".envy-cache-local", ".envy-cache-shared")):
            self.skipTest("a marker or ENVY_CACHE_ROOT is deliberately overriding")
        resolved = envy(self.cfg.repo_root, "cache", "--root")
        self.assertEqual(0, resolved.returncode, resolved.stderr)
        root = Path(resolved.stdout.strip()).resolve()
        self.assertEqual((self.cfg.repo_root / "out/.envy").resolve(), root)

    def test_the_mode_markers_can_never_be_committed(self) -> None:
        """.gitignore lists both cache marker files."""
        ignored = (self.cfg.repo_root / ".gitignore").read_text(encoding="utf-8")
        for marker in (".envy-cache-local", ".envy-cache-shared"):
            self.assertIn(f"/{marker}", ignored.splitlines())

    def test_the_tracked_launchers_are_all_one_schema(self) -> None:
        """The tracked envy launchers for every platform share one schema.
        Regenerate with `./bin/envy deploy --platform all`."""
        schemas: dict[str, set[str]] = {}
        for script in sorted((self.cfg.repo_root / "bin").iterdir()):
            if not script.is_file():
                continue
            for line in script.read_text(encoding="utf-8",
                                         errors="replace").splitlines()[:4]:
                if "envy-managed schema" in line:
                    schemas.setdefault(line.split('"')[1], set()).add(script.name)
                    break
        self.assertTrue(schemas, "no envy-managed launchers found under bin/")
        self.assertEqual(1, len(schemas), f"mixed launcher schemas: {schemas}")

    def test_a_new_conductor_workspace_shares_the_cache(self) -> None:
        """Conductor's setup runs `envy cache --shared`; no .worktreeinclude exists."""
        settings = self.cfg.repo_root / ".conductor/settings.toml"
        self.assertTrue(settings.is_file(), ".conductor/settings.toml is missing")
        self.assertIn("envy cache --shared",
                      settings.read_text(encoding="utf-8"))
        self.assertFalse((self.cfg.repo_root / ".worktreeinclude").exists(),
                         "a copied marker would contradict the setup script")


if __name__ == "__main__":
    unittest.main()
