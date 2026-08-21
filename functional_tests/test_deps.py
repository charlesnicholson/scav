#!/usr/bin/env python3
"""`scav deps` writes the document network as a depfile. The last test is the
one that matters: a real ninja build consuming a real depfile."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scavtest  # noqa: E402

VAC = Path("test_data/charts/vac.scav")


class TestDeps(unittest.TestCase):
    cfg: scavtest.Config
    exe: Path
    scratch: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = scavtest.load_config()
        name = "scav.exe" if os.name == "nt" else "scav"
        cls.exe = cls.cfg.build_dir / "bin" / name
        cls.scratch = scavtest.fresh_dir(cls.cfg.scratch_dir / "deps")

    def run_scav(self, *args: scavtest.Arg) -> subprocess.CompletedProcess[str]:
        argv = [str(self.exe), *[str(a) for a in args]]
        print(f"+ {' '.join(argv)}", flush=True)
        return subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=self.cfg.repo_root,
        )

    def write(self, name: str, text: str) -> Path:
        path = self.scratch / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_network_lists_every_document_once(self) -> None:
        result = self.run_scav("deps", VAC.as_posix())
        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode)
        # led.scav is included twice -- once by vac and once by dock -- and is
        # one document, so it appears once.
        self.assertEqual(
            "test_data/charts/vac.scav: test_data/charts/vac.scav "
            "test_data/charts/dock.scav test_data/charts/led.scav\n",
            result.stdout,
        )

    def test_the_target_defaults_to_the_chart_and_is_overridable(self) -> None:
        result = self.run_scav("deps", "--target", "out/vac.svg", VAC.as_posix())
        self.assertEqual(0, result.returncode)
        self.assertTrue(result.stdout.startswith("out/vac.svg: "))

    def test_a_chart_with_no_includes_depends_on_itself(self) -> None:
        # The target is the caller's string verbatim, since a build system has to
        # match it; a dependency is a document name, which is `/`-separated on
        # every transport. On Windows those two are spelled differently.
        chart = self.write("solo.scav", "chart solo {\n  state A,\n}\n")
        result = self.run_scav("deps", chart)
        self.assertEqual(0, result.returncode)
        self.assertEqual(f"{chart}: {chart.as_posix()}\n", result.stdout)

    def test_document_order_is_the_include_graph_not_arrival(self) -> None:
        # A DocId comes from the first include statement naming it, ordered
        # breadth-first, so the line survives however the files were fetched.
        first = self.run_scav("deps", VAC.as_posix()).stdout
        for _ in range(3):
            self.assertEqual(first, self.run_scav("deps", VAC.as_posix()).stdout)

    def test_a_space_in_a_path_is_escaped(self) -> None:
        chart = self.write("has space.scav", "chart s {\n  state A,\n}\n")
        result = self.run_scav("deps", chart)
        self.assertEqual(0, result.returncode)
        self.assertIn("has\\ space.scav", result.stdout)

    def test_deps_does_not_gate_on_structural_validity(self) -> None:
        # A duplicate name is `check`'s finding. A build should not lose its
        # dependency edges because a state name is wrong.
        leaf = self.write("dupleaf.scav", "chart leaf {\n  state L,\n}\n")
        chart = self.write(
            "dup.scav",
            'chart dup {\n  include "dupleaf.scav" as l,\n  state A,\n  state A,\n}\n',
        )
        result = self.run_scav("deps", chart)
        self.assertEqual(0, result.returncode)
        self.assertIn(leaf.as_posix(), result.stdout)
        self.assertEqual(1, self.run_scav("check", chart).returncode)

    def test_a_missing_document_is_an_error_with_no_output(self) -> None:
        chart = self.write("missing.scav", 'chart m {\n  include "gone.scav" as g,\n}\n')
        result = self.run_scav("deps", chart)
        self.assertEqual(2, result.returncode)
        self.assertEqual("", result.stdout)
        self.assertIn("cannot read", result.stderr)

    def test_ninja_reruns_a_rule_when_an_included_document_changes(self) -> None:
        """A real build consuming a real depfile."""
        build = scavtest.fresh_dir(self.scratch / "ninja")
        (build / "leaf.scav").write_text(
            "chart leaf {\n  state L,\n}\n", encoding="utf-8"
        )
        (build / "root.scav").write_text(
            'chart root {\n  include "leaf.scav" as l,\n  state R,\n}\n',
            encoding="utf-8",
        )
        # Ninja runs a command through a shell on POSIX and through CreateProcess
        # on Windows, so `>` and `&&` reach the program as ordinary arguments
        # there. The rule is one process that does its own redirection.
        (build / "render.py").write_text(
            "import subprocess, sys\n"
            "scav, src, out = sys.argv[1:4]\n"
            'with open(out, "w", encoding="utf-8") as f:\n'
            '    subprocess.run([scav, "dump", src], stdout=f, check=True)\n'
            'with open(out + ".d", "w", encoding="utf-8") as f:\n'
            '    subprocess.run([scav, "deps", "--target", out, src],\n'
            "                   stdout=f, check=True)\n",
            encoding="utf-8",
        )
        # The build statement stays relative to the build directory: ninja reads
        # `:` there as the output separator, so a `D:/...` output is a syntax
        # error rather than a path.
        # `dump` stands in for a renderer: what matters is that ninja learns the
        # second dependency from the depfile, not from the build description.
        (build / "build.ninja").write_text(
            f"""rule render
  command = {self.cfg.python} {build / "render.py"} {self.exe} """
            f"""{build / "root.scav"} $out
  depfile = $out.d
  description = render $out

build root.txt: render root.scav
""",
            encoding="utf-8",
        )

        ninja = Path(self.cfg.make_program)
        env = scavtest.env_without_suppressions()
        self.assertEqual(0, scavtest.run([ninja, "-C", build], env=env).returncode)
        first = (build / "root.txt").read_text(encoding="utf-8")
        self.assertIn("state L", first)
        # The depfile names the included document, which is the whole point.
        self.assertIn("leaf.scav", (build / "root.txt.d").read_text(encoding="utf-8"))

        # Nothing changed, so nothing reruns: without this the next assertion
        # would pass for the wrong reason.
        second = scavtest.run([ninja, "-C", build], env=env)
        self.assertEqual(0, second.returncode)
        self.assertIn("no work to do", second.stdout)

        # The root is untouched; only the document it includes moved.
        (build / "leaf.scav").write_text(
            "chart leaf {\n  state Renamed,\n}\n", encoding="utf-8"
        )
        third = scavtest.run([ninja, "-C", build], env=env)
        self.assertEqual(0, third.returncode)
        self.assertNotIn("no work to do", third.stdout)
        self.assertIn("state Renamed", (build / "root.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
