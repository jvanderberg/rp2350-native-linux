#!/usr/bin/env bash
# board-session.sh LOG [CMD...]: one board test session for tcc: take the
# board lock, write the romfs and the kernel (board-load.sh), log in, set
# tcc up from the romfs, run each CMD (default: hello world, the repo's
# tcc tests with board-run.sh, the build.sh of extras/games, samples and
# pulse), release the lock. The console goes to LOG; each CMD's exit
# status is printed as @@R<status>.
set -u
cd "$(dirname "$0")/.."
PORT=/dev/cu.usbmodemnative1
OWNER=${OWNER:-tcc}
LOG=$1; shift
if [ $# = 0 ]; then
	set -- \
		"cat /proc/meminfo | head -3; uname -a" \
		"cd /tmp && printf '#include <stdio.h>\\nint main(void) { printf(\"hello from tcc\\\\n\"); return 0; }\\n' > hello.c && cc hello.c && ./a.out && tcc -v" \
		"cd /tmp/test && sh board-run.sh" \
		"cd /tmp/extras/games && sh -x ./build.sh stackfall" \
		"cd /tmp/extras && cat /proc/uptime && games/build.sh && samples/build.sh && pulse/build.sh && cat /proc/uptime && ls -l games/stackfall games/rain games/asteroids games/life pulse/pulse samples/bogomips" \
		"cd /tmp/extras/samples && ./bogomips | tail -2"
fi
tools/board-lock.sh acquire $OWNER 7200 || exit 1
trap 'tools/board-lock.sh release $OWNER' EXIT
start=$(date +%s)
tcc/board-load.sh > "$LOG.load" 2>&1 || { echo "load failed"; tail -5 "$LOG.load"; exit 1; }
python3 tools/boot-wait.py "$start" --port $PORT --timeout 90 --log "$LOG.boot" ||
	{ echo "boot failed"; tail -20 "$LOG.boot"; exit 1; }
args=(--timeout "${TMO:-600}" "" "root" "# ")
# setup.sh from the romfs: mount it by hand first (see board-image.sh)
args+=("cat /proc/mtd; d=\$(sed -n 's/^\\(mtd[0-9]*\\):.*tcc\"\$/\\1/p' /proc/mtd); mkdir -p /mnt/tcc && mount -t romfs \$d /mnt/tcc && sh /mnt/tcc/setup.sh; echo @@R\$?; echo @@X\$((7*6))" "@@X42")
for c in "$@"; do
	args+=("$c; echo @@R\$?; echo @@X\$((7*6))" "@@X42")
done
python3 tools/serial-chat.py $PORT "${args[@]}" > "$LOG" 2>&1
echo "serial-chat exit $?"
