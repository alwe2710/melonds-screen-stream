Vendored copy of `core/` from https://github.com/alwe2710/finlink (branch
`main`, commit `b93362a`) -- the portable C99 library behind
the finlink WebSocket streaming protocol also used by the sibling
dolphin-gba-stream and azahar forks, and by melonDS's own bottom-screen
streaming server (`src/streaming/`).

Committed directly rather than as a git submodule, matching how this
codebase already vendors other small C libraries (`src/sha1/`,
`src/xxhash/`) instead of using `externals/`/submodules, which melonDS
doesn't have.

To re-sync after a change upstream: copy `core/include/finlink/`,
`core/src/*.c` and `core/third_party/` (both `miniz/` and `teeny-sha1/` --
the latter is only used by finlink_core's own client-side WS handshake
helpers, unused by this server, but the file still needs to compile as part
of `websocket.c`'s translation unit) over the matching directories here,
update the commit hash above, and rebuild.
