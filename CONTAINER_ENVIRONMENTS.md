# Files container support

Files owns `outer-files-helper` in [Helper](Helper/README.md). The backend handles host operations directly and reuses long-running container helpers over stdin/stdout. Outer Shell supplies container discovery and installation, not an application-operation proxy.

Container images must install `/usr/local/bin/outer-files-helper`. The bundled base images include each app's separate executable. Custom images can COPY the app's static helper directly. Release, deployment and development scripts build it from this app's Helper directory. The container process exits when its backend connection closes; it is not a separately managed daemon.

The HTML frontend uses the container-aware APIs. Outerframe behavior is unchanged. Docker is tested; Apple container/wslc still require live validation. Native Windows backend integration remains separate.

On macOS, discovery refreshes the user’s launchd API socket after Outer Shell restarts. Root Files discovers user registries and validates each user’s local Docker socket. The system daemon has no default container provider; explicit custom registry sockets remain supported.

## Places

The HTML Places strip uses binary operation 10. Files' helper returns Home and directory mounts from Linux mountinfo (excluding the root filesystem and /proc, /sys, /dev). Regular file mounts are omitted. Mount paths are decoded and canonical paths deduplicated.

Images can install `/etc/outer-files/places.tsv`: one absolute directory path, a tab, and a display name per line. Explicit entries rename discovered mounts or add directories; nonexistent directories are omitted. Example: `/work/src/blocks` followed by a tab and `probablymarcus.com`. No Dockerfile parsing or JSON is involved. Updated container helpers are required for Places discovery.

Folder requests reuse cached container resolution and persistent helpers. The visible HTML app refreshes discovery every ten seconds and on returning to the foreground; cache entries expire after thirty seconds. Missing/dead connections force discovery, and transport failures revalidate immutable container identity before one read-only retry. Explicit discovery prunes stopped or replaced sessions.
