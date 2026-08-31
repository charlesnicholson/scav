#!/usr/bin/env python3
"""Every tool the configured tree recorded came from envy, not from whatever the
machine had. Opt-in, because envy is explicitly not a build prerequisite.

The sandbox itself is not opt-in and is checked unconditionally below: the
manifest asks for a cache under out/ so that removing that one directory removes
every tool, package and build artifact."""

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
        # Asked of envy, not guessed. envy resolves this from four tiers -- an
        # absolute ENVY_CACHE_ROOT, a `--local`/`--shared` marker, `@envy
        # cache-mode`, then `@envy cache-local` -- and reimplementing that here
        # is how a test starts disagreeing with the thing it is testing.
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
    """`rm -rf out` is a factory reset. That is a promise to anyone who builds
    scav once and does not want a toolchain left on their machine, so it is
    checked whether or not envy is required."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        cls.manifest = (cls.cfg.repo_root / "envy.lua").read_text(encoding="utf-8")

    def test_the_manifest_asks_for_a_cache_under_out(self) -> None:
        # A property of the manifest, not of this invocation, so it is checked as
        # text: a marker or an override must not hide a manifest that changed the
        # default for everyone.
        self.assertIn('-- @envy cache-local "out/.envy"', self.manifest)

    def test_the_manifest_does_not_use_the_removed_directives(self) -> None:
        """cache-posix/cache-win are errors in envy 0.2.1, not synonyms. They held
        absolute paths and needed shell expansion, which four readers implemented
        four ways -- the bug cache-local exists to remove."""
        for gone in ("cache-posix", "cache-win"):
            self.assertNotIn(gone, self.manifest)

    def test_naming_the_tree_is_what_selects_local_mode(self) -> None:
        """envy defaults to the user-wide cache with no directives at all, so the
        sandbox is `cache-local` doing its job. A `cache-mode "shared"` line would
        silently undo it, which is why its absence is asserted rather than assumed."""
        self.assertNotIn("cache-mode", self.manifest)

    def test_the_resolved_root_is_under_out_on_a_clean_checkout(self) -> None:
        """The manifest is only half the claim; this is envy agreeing with it.

        Skipped when a marker or an override is in play, because then the
        developer has deliberately said otherwise and that is the feature.
        """
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
        """A marker records one machine's preference. Committing one would hand
        every other checkout a cache location it never asked for. git decides
        that, not a convention."""
        for marker in (".envy-cache-local", ".envy-cache-shared"):
            with self.subTest(marker=marker):
                probe = self.cfg.repo_root / marker
                existed = probe.exists()
                if not existed:
                    probe.write_bytes(b"")
                try:
                    result = subprocess.run(
                        ["git", "check-ignore", "-q", marker],
                        cwd=self.cfg.repo_root, check=False)
                    self.assertEqual(0, result.returncode,
                                     f"{marker} is not gitignored")
                finally:
                    if not existed:
                        probe.unlink()

    def test_the_tracked_launchers_are_all_one_schema(self) -> None:
        """`envy sync` deploys only the host's flavour, so a bump run on one OS
        leaves the other's tracked scripts behind. A stale launcher resolves the
        cache by older rules than the binary it bootstraps, which is the exact
        split `cache-local` was introduced to end. Regenerate with
        `./bin/envy deploy --platform all`."""
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

    def test_a_new_worktree_inherits_the_developers_choice(self) -> None:
        """The markers are the one piece of state a fresh worktree needs and git
        will not carry, so Conductor is told to copy them."""
        include = self.cfg.repo_root / ".worktreeinclude"
        self.assertTrue(include.is_file(), ".worktreeinclude is missing")
        listed = include.read_text(encoding="utf-8")
        for marker in (".envy-cache-local", ".envy-cache-shared"):
            self.assertIn(marker, listed)


if __name__ == "__main__":
    unittest.main()
