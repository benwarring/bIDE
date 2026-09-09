<p align="center">
  <img src="assets/header.svg" alt="bIDE — a terminal text editor written in C" width="880">
</p>

<p align="center">
  <a href="#building"><img alt="language" src="https://img.shields.io/badge/C-C17-58a6ff?style=flat-square"></a>
  <img alt="platform" src="https://img.shields.io/badge/platform-POSIX-8b949e?style=flat-square">
  <img alt="dependencies" src="https://img.shields.io/badge/dependencies-none-28c840?style=flat-square">
</p>

---

**bIDE** is a modeless terminal text editor built from nothing but libc and the
terminal itself — no ncurses, no JSON library, no dependencies at all. It draws
with raw escape codes, speaks the Language Server Protocol over pipes, and its
whole event loop is one `poll()`.

It started as a read-only file viewer and grew into something that can actually
edit code: multiple buffers, undo, multi-cursor, incremental search, syntax
highlighting, and language-server-backed completion, diagnostics and go-to-definition.

## Features

| | |
|---|---|
| **Editing** | insert, delete, save; CRLF preserved on round-trip; tabs rendered to a 4-column stop |
| **Undo/redo** | coalesced into runs, so a typed word undoes as a word |
| **Multi-cursor** | add cursors above/below, or at the next occurrence of the word under the cursor |
| **Search** | incremental, wrapping, with the match highlighted as you type |
| **Syntax** | C and Python — keywords, types, strings, numbers, line and block comments |
| **Buffers** | open many files, cycle, close, or jump via a fuzzy switcher |
| **Command palette** | fuzzy-searchable, and it doubles as the keymap documentation |
| **File picker** | fuzzy open across a cached recursive scan of the tree |
| **LSP** | completion, hover, diagnostics, go-to-definition and a jump-back stack, over `clangd` and `pylsp` |
| **Mouse** | click to position the cursor, wheel to scroll |

Async work never blocks the keyboard: the loop waits on stdin *and* every
language server pipe at once, sleeps exactly as long as the nearest pending
thing, and only redraws when something actually changed.

## Building

Needs `gcc` (or `clang`), `make`, and a POSIX terminal — Linux, macOS, or WSL.

```sh
make          # -> bin/main
./bin/main src/main.c
```

Optional extras:

```sh
make compdb   # compile_commands.json, so clangd resolves the project's own headers
make asan     # rebuild with AddressSanitizer + UBSan
make clean
```

To get a `bide` command on your PATH, run the installer once:

```sh
sh scripts/start.sh          # builds, then installs to ~/.local/bin/bide
BINDIR=/usr/local/bin sh scripts/start.sh   # or somewhere else
```

Then `bide file.c` works from any directory. It installs a two-line wrapper
rather than a symlink, so it keeps pointing at the current binary after you
rebuild. Note that bIDE writes its LSP transcript to `lsp.log` in whatever
directory you launch it from.

Language servers are optional and detected by file extension. Install
[`clangd`](https://clangd.llvm.org/) for C and
[`pylsp`](https://github.com/python-lsp/python-lsp-server) for Python; without
them everything else still works.

## Keys

Press <kbd>Ctrl</kbd>+<kbd>K</kbd> for the palette — it lists every command with
its binding, which is the authoritative version of this table.

| Key | | Key | |
|---|---|---|---|
| <kbd>Ctrl</kbd>+<kbd>S</kbd> | Save | <kbd>Ctrl</kbd>+<kbd>G</kbd> | Go to definition |
| <kbd>Ctrl</kbd>+<kbd>O</kbd> | Open file… | <kbd>Ctrl</kbd>+<kbd>T</kbd> | Jump back |
| <kbd>Ctrl</kbd>+<kbd>F</kbd> | Find in file | <kbd>Ctrl</kbd>+<kbd>L</kbd> | Hover docs |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Y</kbd> | Undo / redo | <kbd>Ctrl</kbd>+<kbd>Space</kbd> | Complete |
| <kbd>Ctrl</kbd>+<kbd>N</kbd> / <kbd>P</kbd> | Next / prev buffer | <kbd>Ctrl</kbd>+<kbd>E</kbd> / <kbd>A</kbd> | Next / prev diagnostic |
| <kbd>Ctrl</kbd>+<kbd>B</kbd> | Switch buffer… | <kbd>Ctrl</kbd>+<kbd>D</kbd> | Cursor at next match |
| <kbd>Ctrl</kbd>+<kbd>W</kbd> | Close buffer | <kbd>Ctrl</kbd>+<kbd>↑</kbd> / <kbd>↓</kbd> | Add cursor above / below |
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Command palette | <kbd>Esc</kbd> | Collapse to one cursor |
| <kbd>Ctrl</kbd>+<kbd>X</kbd> | Quit | <kbd>Ctrl</kbd>+<kbd>R</kbd> | Rescan files |

Rebinding is editing the `BINDINGS` table in [`src/command.c`](src/command.c).
There is no config file, by design.

## Layout

```
include/          public headers, one per module
src/
  main.c          poll loop and startup
  terminal.c      raw mode, window size, SIGWINCH
  buffer.c        rows, files, open buffers
  input.c         key decoding and editing operations
  render.c        the screen, status bar, prompts
  command.c       command table, keymap, palette
  undo.c          undo/redo stacks
  search.c        incremental find
  syntax.c        highlight rules for C and Python
  lsp.c           server processes, framing, requests
  lspfeat.c       completion, hover, diagnostics, definition
  json.c          the JSON parser/writer LSP needs
  popup.c         floating windows
  fuzzy.c         subsequence scoring
  list.c          the scrolling list widget
  files.c         recursive file scan
  wrappers.c      syscall wrappers that die loudly
```

## Status

Working editor, still growing. Split panes are the next structural step — the
`View`/`Buffer` split already exists to make it additive rather than a rewrite.
