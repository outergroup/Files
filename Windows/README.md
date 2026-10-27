# Files on Windows

`FilesBackend.c` serves the existing FilesWeb UI over the canonical per-user
named pipe. Its binary listing encoder is shared with the Unix backend in
`Backend/FileList.h`; pipe/HTTP transport comes from the native C Windows runtime
in Backends. It builds and installs with `Backends/Windows/build.ps1`; see
[Outer Shell Windows](../../Backends/Windows/README.md) for setup and constraints.

The host uses Windows known folders for Home/Desktop and local-drive paths such
as `C:/Users/name`. It supports listings, previews, downloads, folder creation,
and uploads. UNC/device paths and alternate data stream syntax are rejected;
mounted shares remain accessible through their drive paths. Open With is not
implemented in this first Windows port.
