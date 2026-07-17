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

## Remote Development

To test a deployed remote host with curl:

```bash
ssh "$HOST" 'curl --unix-socket "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/org.outershell.Files" http://localhost/'
```
