# Run doc — FriendFinder web preview (this thread)

## Reproduce the artifacts

The Preview tab serves a single self-contained file. Regenerate it after ANY
edit to `web/index.html`, `web/style.css`, or `web/app.js`:

```bash
gcc -std=c99 -Wall -Wextra -Werror -O2 -o bundle_web.exe tools/bundle_web.c
./bundle_web.exe          # -> web/index.standalone.html (inlines CSS+JS)
```

The three source files in `web/` remain the canonical frontend; the
standalone file is build output. `bundle_web` escapes `</` as `<\/` inside
the inlined script so template literals survive single-file inlining.

## Run the server

No dev server, port, or dependencies are needed for the preview: the
registered file is static HTML (`register_preview` with `htmlPath`). The
preview re-reads the file on reload, so the loop is: edit `web/` → run
`./bundle_web.exe` → reload the Preview tab.

## Notes

- `USE_API = true` in `web/app.js`: every read goes to the C HTTP backend
  (`server.c` -> graph.c / recommend.c). The JS seed/mock remains in the
  file only as a documented offline fallback and is not used.
- Run the backend alongside the preview:

  ```bash
  gcc -std=c99 -Wall -Wextra -Werror -O2 -o friend_server.exe server.c graph.c recommend.c -lws2_32
  ./friend_server.exe        # listens on http://localhost:8080, loads data/sample_network.txt
  ```

  (Or `make serve` where make exists.) The server also serves `web/` at
  `http://localhost:8080/` as an alternative to the standalone preview.
- API: `/api/users`, `/api/friends?user=ID`, `/api/recommend?user=ID&top=N`,
  `/api/network-summary`. Recommendations come verbatim from
  `recommend_friends()` in recommend.c (bounded 2-hop BFS).
