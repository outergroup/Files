# Files helper

`outer-files-helper` belongs to Files; source and protocol evolve in this repository. It does not run as an Outer Shell service. Install it at `/usr/local/bin/outer-files-helper` in container images. Build with `Helper/build.sh OS ARCH OUTPUT`. Linux builds are static musl; macOS uses the native compiler. No Python or Go runtime is needed.

The backend links `Backend/Environment.c` directly. Discovery, connection management, local operations and directory browsing run in the backend process. Host-only implementation lives in `Backend/Environment/`; common protocol and local-operation code is compiled from Helper. There is no host-helper process, host mode, or backend-to-host-helper protocol.

The backend directly runs `outer-files-helper serve` through the container runtime's `exec -i` and keeps up to 32 persistent connections keyed by executable, runtime endpoint, immutable container ID and user. It evicts least-recently-used connections at capacity and prunes unregistered/stopped/replaced selections on discovery. Each app and backend scope own their connections. Top only opens a container connection when its host process snapshot cannot see that container's processes.

Container helpers wait for multiple sequential requests without idle expiry. EOF shuts them down. Backend shutdown closes all pipes and reaps runtime children; backend crashes close the pipes through the OS. Child processes do not inherit listening sockets or unrelated client descriptors. Requests have bounded I/O deadlines. Failed read-only operations can reconnect once; writes and signals are never automatically replayed after ambiguous transport failure. Ordinary operation errors keep the connection usable. Backend environment operations are serialized, including concurrent callers.

The HTTP API still uses FLCQ requests and FLCR replies, decoded by Resources/FilesWeb/environment.js. These bytes are handled in-process.

Container frames: `FLH2`, u32 kind, u32 request ID, u32 size; at most 65536 payload bytes. Hello advertises version 2, this app's capabilities and limits. Metadata/data/end frames form each request/reply. Request metadata begins `FLHR`, reply metadata `FLHS`; layouts are declared in binary.h and binary.c. All integers are little-endian, string references are absolute u32 offset/byte-length pairs, file bodies are raw bytes. Different apps reject each other's protocol. There is no shared universal helper executable or protocol.

Container operations: directory listing and raw reads/previews.

Run `python3 Helper/test_app_helper.py HELPER BACKEND` for real backend/stream tests covering reuse, process crashes, container identity changes and cleanup. The source tests use Python; the shipped helper does not. Docker is live-tested. Apple container and wslc execution paths still need live validation.
