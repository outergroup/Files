# Files

Files is a minimal outerframe app for browsing a directory over the same backend-backed shape as the other quick Outer Loop prototypes. The first version intentionally keeps the UI small: it lists the current home directory, supports scroll/selection, opens folders, and exposes enough pasteboard capability surface to start iterating on drag-and-drop download/upload behavior.

## Build

```bash
./build_run.sh
```

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
  --label dev.outergroup.Files \
  --bundles-dir ./build/run/bundles
```

You can also pass an explicit socket:

```bash
./build/macos/Release/FilesBackend \
  --socket-path "$XDG_RUNTIME_DIR/dev.outergroup.Files" \
  --label dev.outergroup.Files \
  --bundles-dir ./build/run/bundles
```

The backend serves the outerframe descriptor, the archived macOS content bundles, and `/api/files?path=...` from the same loopback HTTP server.

To create a release payload for a Home Screen-style installer, build both Linux
backend architectures into `build/linux-package/RemoteLinuxBinaries`, then run:

```bash
./Scripts/package_release.sh
```

The archive is written to `build/release/Files.tar.gz`. Deployment-specific
publishing should live outside this repository.

## Remote Development

This public repository intentionally does not contain host-specific deployment
scripts. Private lab deployments should live in an external deployment
workspace.

To test a deployed remote host with curl:

```bash
ssh "$HOST" 'curl --unix-socket "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/dev.outergroup.Files" http://localhost/'
```
