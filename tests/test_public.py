"""Проверка публикации: принудительно добавленный артефакт и утечка в предке коммита."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "check_public.py"


class PublicTreeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.git("init", "-q")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.invalid")
        self.put("README.md", b"Own source\n")
        self.git("add", "README.md")
        self.git("commit", "-qm", "clean")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

    def put(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def check_tree(self, *args):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=self.root, capture_output=True, text=True)

    def test_clean_source(self):
        self.put("runtime/game/menu.c", b"/* Authored source, not a data directory. */\n")
        self.git("add", ".")
        self.assertEqual(self.check_tree().returncode, 0)
        self.assertEqual(self.check_tree("--history").returncode, 0)

    def test_forced_ignored_artifact_and_history(self):
        self.put(".gitignore", b"game/\n")
        self.put("game/System/Game.EXE", b"MZ\0synthetic fixture")
        self.git("add", ".gitignore")
        self.git("add", "-f", "game/System/Game.EXE")
        self.assertEqual(self.check_tree().returncode, 1)
        self.git("commit", "-qm", "artifact")
        self.git("rm", "-q", "game/System/Game.EXE")
        self.git("commit", "-qm", "removed")
        self.assertEqual(self.check_tree("HEAD").returncode, 0)
        result = self.check_tree("--history")
        self.assertEqual(result.returncode, 1)
        self.assertIn("game/System/Game.EXE", result.stdout)

    def test_generated_code_and_renamed_binary(self):
        self.put("runtime/copied.c", "/* Сгенерировано recomp из Core.dll. */\n".encode())
        self.put("docs/data.txt", b"renamed\0binary")
        self.git("add", ".")
        result = self.check_tree()
        self.assertEqual(result.returncode, 1)
        self.assertIn("runtime/copied.c", result.stdout)
        self.assertIn("docs/data.txt", result.stdout)


if __name__ == "__main__":
    unittest.main()
