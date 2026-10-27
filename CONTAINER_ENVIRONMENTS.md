# Files container support

Files owns `outer-files-helper` in [Helper](Helper/README.md). The backend handles host operations directly and reuses long-running container helpers over stdin/stdout. Outer Shell supplies container discovery and installation, not an application-operation proxy.

Container images must install `/usr/local/bin/outer-files-helper`. The bundled base images include each app's separate executable. Custom images can COPY the app's static helper directly. Release, deployment and development scripts build it from this app's Helper directory. The container process exits when its backend connection closes; it is not a separately managed daemon.

The HTML frontend uses the container-aware APIs. Outerframe behavior is unchanged. Docker is tested; Apple container/wslc still require live validation. Native Windows backend integration remains separate.

