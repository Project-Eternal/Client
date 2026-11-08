#!/bin/bash
# The test environment for runs of the web client without a person at the keyboard.
#   env.sh up                 game files over HTTP (8082), the WebSocket proxy (8090); checks the standard server's containers
#   env.sh serve DIR PORT     a build (a publish/ directory) on a port, with the headers the page needs and no caching
#   env.sh status | down
# Environment: GAME_DIR (the 3.3.5a installation, default ~/Games/WoW335a), TEST_WORK (default ~/.cache/wow-test), SERVER_DIR (the standard server's
# compose directory, default ~/wow-eternal-server/src).
set -u
here=$(cd "$(dirname "$0")" && pwd)
GAME_DIR=${GAME_DIR:-$HOME/Games/WoW335a}
WORK=${TEST_WORK:-$HOME/.cache/wow-test}
SERVER_DIR=${SERVER_DIR:-$HOME/wow-eternal-server/src}
FILES_PORT=8082; PROXY_PORT=8090
mkdir -p "$WORK"
running() { [ -f "$WORK/$1.pid" ] && kill -0 "$(cat "$WORK/$1.pid")" 2>/dev/null; }
case "${1:-status}" in
  up)
    mkdir -p "$WORK/gamefiles" && ln -sfn "$GAME_DIR/Data" "$WORK/gamefiles/Data"
    for exe in "$GAME_DIR"/*.exe "$GAME_DIR"/*.EXE; do [ -f "$exe" ] && ln -sfn "$exe" "$WORK/gamefiles/$(basename "$exe")"; done
    python3 "$here/../make_data_manifest.py" "$WORK/gamefiles" >/dev/null
    running files || { nohup node "$here/serve-gamefiles.js" "$WORK/gamefiles" $FILES_PORT > "$WORK/files.log" 2>&1 & echo $! > "$WORK/files.pid"; }
    running proxy || { nohup node "$here/../proxy/wow-proxy.js" --port $PROXY_PORT --host 127.0.0.1 --allow-private --map 10.99.0.2=127.0.0.1 > "$WORK/proxy.log" 2>&1 & echo $! > "$WORK/proxy.pid"; }
    sleep 1; "$0" status ;;
  serve)
    dir=$(cd "$2" && pwd); port=$3; name=wow-test-$port
    sed 's/max-age=3600/max-age=0/' "$here/../../docker/nginx.conf" > "$WORK/nginx.conf"
    docker rm -f "$name" >/dev/null 2>&1
    docker run -d --name "$name" -p "$port:8080" -v "$dir:/usr/share/nginx/html:ro" -v "$WORK/nginx.conf:/etc/nginx/conf.d/default.conf:ro" nginx:1.27-alpine >/dev/null && echo "http://localhost:$port/" ;;
  down)
    for n in files proxy; do running $n && kill "$(cat "$WORK/$n.pid")"; rm -f "$WORK/$n.pid"; done
    docker ps --format '{{.Names}}' | grep '^wow-test-' | xargs -r docker rm -f >/dev/null; echo stopped ;;
  status)
    running files && echo "game files: http://localhost:$FILES_PORT/ ($(curl -s -o /dev/null -w '%{http_code}' localhost:$FILES_PORT/manifest.txt))" || echo "game files: not running"
    running proxy && echo "proxy: ws://localhost:$PROXY_PORT/" || echo "proxy: not running"
    for c in ac-database ac-authserver ac-worldserver; do docker ps --format '{{.Names}}' | grep -qx $c && echo "$c: up" || echo "$c: DOWN (cd $SERVER_DIR && docker compose up -d)"; done
    docker ps --format '{{.Names}} {{.Ports}}' | grep '^wow-test-' | sed 's/->8080.*//' ;;
esac
