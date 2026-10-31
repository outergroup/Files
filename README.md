# Files

Files is a minimal outerframe app for browsing a directory over the same backend-backed shape as the other quick Outer Loop prototypes. The first version intentionally keeps the UI small: it lists the current home directory, supports scroll/selection, opens folders, and exposes enough pasteboard capability surface to start iterating on drag-and-drop download/upload behavior.

## Build

```bash
./build_run.sh
```

## Deploy Over SSH

With a current Outer Shell installed on the target:

```bash
./app target "ssh -p 22 you@server"
./app deploy
```

The deploy target is stored in the gitignored `target.env`. Deployment detects
the target architecture and libc and builds only the matching dynamic backend.
glibc builds use the manylinux2014 (glibc 2.17) baseline; musl builds use
musllinux 1.2. Run `./app build-matrix` to build all four Linux release
variants, or `./app help` for the other development commands.

## Run

```bash
PORT=7354
./build/macos/Release/FilesBackend --port "$PORT" --bundles-dir ./build/run/bundles
```

Open this URL in Outer Loop or Outer Frame:

```text
http://127.0.0.1:7354/
```

For Outer Loop-managed deployments, prefer a Unix socket. If `--port` is omitted, Files listens directly under `$XDG_RUNTIME_DIR` using the backend label:

```bash
./build/macos/Release/FilesBackend \
  --label org.outershell.Files \
  --bundles-dir ./build/run/bundles
```

You can also pass an explicit socket:

```bash
./build/macos/Release/FilesBackend \
  --socket-path "$XDG_RUNTIME_DIR/org.outershell.Files" \
  --label org.outershell.Files \
  --bundles-dir ./build/run/bundles
```

The backend serves the outerframe descriptor, the archived macOS content bundles, `/api/files?path=...`, and `/api/openers?path=...` from the same loopback HTTP server. Files queries outershelld's socket API directly for opener registry entries, using `OUTERSHELLD_API_SOCKET` when set or the platform default API socket path otherwise.

To create a release payload for a Home Screen-style installer, build both Linux
backend architectures into `build/linux-package/RemoteLinuxBinaries`, then run:

```bash
./Scripts/package_release.sh
```

The archive is written to `build/release/Files.tar.gz`. Deployment-specific
publishing should live outside this repository.

## Favorites

In either the HTML or Outerframe app, on the host, use **Add to Favorites** beside the current folder path, or
right-click a folder and choose **Add to Favorites**. Saved folders appear in
the places bar immediately and persist across restarts. Existing favorites
show **In Favorites** to prevent duplicates.

Personal favorites are stored as `path<TAB>label` rows in
`$XDG_CONFIG_HOME/outer-files/places.tsv`, defaulting to
`~/.config/outer-files/places.tsv` for the backend's user. Shared entries in
`/etc/outer-files/places.tsv` are still loaded. Adding favorites in containers
is not yet supported.

Run the persistence tests with a compiled `Tests/EnvironmentDriver.c` plus
`Backend/Environment.c` driver:

```bash
python3 Scripts/test_favorites.py /path/to/environment-driver
```

## Remote Development

To test a deployed remote host with curl:

```bash
ssh "$HOST" 'curl --unix-socket "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/org.outershell.Files" http://localhost/'
```

### Requester context

Files sends its own `X-Files-User` header on API requests. The web frontend reads
`window.outerLoop.sessionContext` when hosted in Outer Loop; the native
outerframe frontend uses `InitializeContentArguments.sessionUsername`. This is the
session's SSH username (or local username in local mode), not proxy credentials.
Without host context, Files omits the header and the backend uses its existing
process-user fallback. The value supplies permission-display and file-opener
context; it is not authentication proof.

### Container runtime adapters

Registered Docker, Apple Container and `wslc` targets use explicit runtime argv
adapters. The helper protocol remains binary and the same app-owned helper can
run in either Linux container runtime. Root macOS backends enter the owning
user's launchd session before invoking Apple Container. Helper image builds accept
`OUTER_BUILD_CONTAINER_RUNTIME=container|docker` and auto-detect an available
builder. `Scripts/test_runtime_command.c` tests the command contract, including
`wslc.exe`; Windows still needs native process transport integration and live
validation. `wslc` is not treated as a `wsl.exe` distribution.


## Windows

Windows host folders, known-folder places and favorites, previews, and Docker/WSLc container browsing use the native named-pipe backend. Plaintext opening preserves the selected container target.
Build and packaging are integrated in `../Backends/Windows/build.ps1`; install the
app through Outer Shell’s Add Apps experience. The package includes HTML content
and macOS native outerframe bundles.
