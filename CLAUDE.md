@AGENTS.md

## Claude Code
- On Windows, commands run through the Bash tool lose one level of backslashes, even in quoted
  heredocs: write edit scripts with the Write tool and run the file. The Write tool turns the escape
  "backslash-u-0000" into a NUL byte (avoid it; after bulk edits scan for NUL bytes). A Python
  script that writes C needs `'\\0'` and `"\\n"`; Python edits of UTF-8 files need
  `encoding='utf-8'`, and writes need `newline='\n'` (the repository uses LF). Run `idf.py` only
  through PowerShell.
- An agent that runs in a worktree (`.claude/worktrees`, git-ignored) leaves it behind whenever it
  changed files: a full checkout plus its builds. Once its work is applied or dropped, `python
  tools/backlog.py worktrees --clean <name>` keeps its changes in `local-archive/worktree-patches`
  and removes it with its branch.

# Compact instructions
When compacting, keep the request and the decisions of the person you work for, the files changed,
the checks run and their results, what is left and the next step. Drop file contents, command output
and anything already in HANDOFF.local.md, the backlog or the feature documents.
