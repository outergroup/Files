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

The backend serves the outerframe descriptor, the archived macOS content bundles, and `/api/files?path=...` from the same loopback HTTP server.

## Deploy to Pircus

```bash
./deploy_pircus.sh
```

This builds the macOS outerframe bundle locally, copies the bundle archives and C backend to `Pircus:~/outerloop-files`, compiles the backend on the Raspberry Pi, and starts it on remote loopback port `7354`.

To test it from this Mac with a plain browser or curl:

```bash
ssh -N -L 7355:127.0.0.1:7354 Pircus
open http://127.0.0.1:7355/
```
