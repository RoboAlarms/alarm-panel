"""backlog.py worktrees --clean: an agent worktree goes, and nothing only it held is lost."""
import argparse
import importlib.util
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("backlog", TOOLS / "backlog.py")
bl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bl)

ENV = {"GIT_AUTHOR_NAME": "Alex", "GIT_AUTHOR_EMAIL": "alex@example.com",
       "GIT_COMMITTER_NAME": "Alex", "GIT_COMMITTER_EMAIL": "alex@example.com"}


def run(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), "-c", "commit.gpgsign=false", *args],
                          capture_output=True, text=True, check=True).stdout


def clean_args(*names, older_than=12.0, dry_run=False):
    return argparse.Namespace(names=list(names), older_than=older_than, dry_run=dry_run)


class CleanWorktrees(unittest.TestCase):
    def setUp(self):
        self.saved_env = {k: os.environ.get(k) for k in ENV}
        os.environ.update(ENV)
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve() / "repo"
        self.root.mkdir()
        run(self.root, "init", "-q", "-b", "main")
        (self.root / "zone.c").write_text("int zones = 8;\n", encoding="utf-8")
        (self.root / ".gitignore").write_text("build/\n", encoding="utf-8")
        run(self.root, "add", "-A")
        run(self.root, "commit", "-q", "-m", "Start")
        self.saved = {k: getattr(bl, k) for k in ("ROOT", "ARCHIVE", "WORKTREES", "PATCHES")}
        bl.ROOT = self.root
        bl.ARCHIVE = self.root / "local-archive"
        bl.WORKTREES = self.root / ".claude" / "worktrees"
        bl.PATCHES = bl.ARCHIVE / "worktree-patches"

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(bl, k, v)
        for k, v in self.saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def add_worktree(self, name):
        path = bl.WORKTREES / name
        run(self.root, "worktree", "add", "-q", "-b", f"worktree-{name}", str(path))
        return path

    def age(self, path, hours):
        """Make the worktree look idle for `hours` (its git files and changed files)."""
        then = time.time() - hours * 3600
        gitdir = Path(run(path, "rev-parse", "--absolute-git-dir").strip())
        for p in [gitdir / "HEAD", gitdir / "index", gitdir / "gitdir"] + bl.changed_files(path):
            if p.exists():
                os.utime(p, (then, then))

    def test_changes_and_new_files_are_kept_then_the_worktree_and_branch_go(self):
        wt = self.add_worktree("agent-a1")
        (wt / "zone.c").write_text("int zones = 16;\n", encoding="utf-8")
        (wt / "new_test.c").write_text("/* ZSRC-001 */\n", encoding="utf-8")
        (wt / "build").mkdir()
        (wt / "build" / "big.bin").write_bytes(b"\0" * 1024)   # ignored: rebuilt, not kept
        self.age(wt, 24)
        self.assertEqual(bl.clean_worktrees(clean_args(), bl.agent_worktrees()), 0)
        self.assertFalse(wt.exists())
        self.assertEqual(run(self.root, "branch", "--list", "worktree-agent-a1").strip(), "")
        patches = list(bl.PATCHES.glob("*-agent-a1.patch"))
        self.assertEqual(len(patches), 1)
        text = patches[0].read_text(encoding="utf-8")
        self.assertIn("int zones = 16;", text)
        self.assertIn("new_test.c", text)
        self.assertNotIn("big.bin", text)
        run(self.root, "apply", "--check", str(patches[0]))     # it applies to main

    def test_a_recently_active_worktree_is_left_alone(self):
        wt = self.add_worktree("agent-busy")
        (wt / "zone.c").write_text("int zones = 32;\n", encoding="utf-8")
        bl.clean_worktrees(clean_args(), bl.agent_worktrees())
        self.assertTrue(wt.exists())
        self.assertEqual(list(bl.PATCHES.glob("*")) if bl.PATCHES.exists() else [], [])

    def test_a_named_worktree_goes_however_recent(self):
        wt = self.add_worktree("agent-done")
        (wt / "zone.c").write_text("int zones = 4;\n", encoding="utf-8")
        bl.clean_worktrees(clean_args("agent-done"), bl.agent_worktrees())
        self.assertFalse(wt.exists())
        self.assertEqual(len(list(bl.PATCHES.glob("*-agent-done.patch"))), 1)

    def test_commits_main_lacks_are_kept_as_format_patch(self):
        wt = self.add_worktree("agent-commit")
        (wt / "zone.c").write_text("int zones = 64;\n", encoding="utf-8")
        run(wt, "commit", "-q", "-am", "Raise the zone count")
        bl.clean_worktrees(clean_args("agent-commit"), bl.agent_worktrees())
        self.assertFalse(wt.exists())
        mails = list(bl.PATCHES.glob("*-agent-commit-commits/*.patch"))
        self.assertEqual(len(mails), 1)
        self.assertIn("Raise the zone count", mails[0].read_text(encoding="utf-8"))

    def test_dry_run_changes_nothing(self):
        wt = self.add_worktree("agent-dry")
        (wt / "zone.c").write_text("int zones = 2;\n", encoding="utf-8")
        bl.clean_worktrees(clean_args("agent-dry", dry_run=True), bl.agent_worktrees())
        self.assertTrue(wt.exists())
        self.assertFalse(bl.PATCHES.exists())

    def test_the_rest_of_a_failed_removal_is_finished_once_its_changes_were_saved(self):
        wt = self.add_worktree("agent-half")
        (wt / "zone.c").write_text("int zones = 128;\n", encoding="utf-8")
        bl.save_worktree(str(wt), "worktree-agent-half", "2026-09-30")
        run(self.root, "worktree", "remove", "--force", str(wt))    # git got part of the way...
        wt.mkdir(parents=True)
        (wt / "left.txt").write_text("a file git couldn't delete\n", encoding="utf-8")  # ...not all
        self.assertEqual(bl.clean_worktrees(clean_args(), bl.agent_worktrees()), 0)
        self.assertFalse(wt.exists())
        self.assertEqual(run(self.root, "branch", "--list", "worktree-agent-half").strip(), "")
        self.assertTrue((bl.PATCHES / "2026-09-30-agent-half.patch").exists())

    def test_stray_folders_go_only_when_empty(self):
        bl.WORKTREES.mkdir(parents=True)
        (bl.WORKTREES / "agent-empty").mkdir()
        full = bl.WORKTREES / "agent-full"
        full.mkdir()
        (full / "notes.txt").write_text("keep me\n", encoding="utf-8")
        bl.clean_worktrees(clean_args(), bl.agent_worktrees())
        self.assertFalse((bl.WORKTREES / "agent-empty").exists())
        self.assertTrue((full / "notes.txt").exists())


if __name__ == "__main__":
    unittest.main()
