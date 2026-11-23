#!/bin/sh
# run-tests.sh [ssh]: smoke tests of the ATRANS programs, on the board or
# under qemu-arm (test-qemu.sh). Every test prints PASS/FAIL; the last line
# is a summary. "ssh" adds a dropbear server + dbclient login over
# 127.0.0.1 (it adds a key to ~/.ssh/authorized_keys for the duration).
D=$(cd "$(dirname "$0")" && pwd)
T=${TMPDIR:-/tmp}/atrans-tests.$$
mkdir -p "$T"
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "PASS $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1${2:+: $2}"; }
# check NAME EXPECTED-SUBSTRING COMMAND...: output (stdout+stderr) must contain it
check() {
  n=$1 want=$2; shift 2
  out=$("$@" 2>&1); rc=$?
  case $out in *"$want"*) ok "$n";; *) bad "$n" "rc $rc: $(echo "$out" | tail -3 | tr '\n' '|')";; esac
}

check hello "hello from an ATRANS program" "$D/hello"
"$D/selftest" > "$T/selftest.out" 2>&1
st=$?
grep -E '^(FAIL|SKIP)' "$T/selftest.out"
[ $st = 0 ] && ok "selftest ($(tail -1 "$T/selftest.out"))" || bad selftest "$(tail -1 "$T/selftest.out")"

check lua "xxx 1024.0 3.14 co:1:2" lua -e '
  local co = coroutine.wrap(function(a) local b = coroutine.yield(a) ; return b end)
  print(("x"):rep(3) .. " " .. 2^10 .. " " .. string.format("%.2f", math.pi) .. " co:" .. co(1) .. ":" .. co(2))'
check "lua os/io" "lua-io-ok" lua -e 'local f = io.popen("echo lua-io-ok") ; io.write(f:read("a")) ; f:close()'
check bc "18446744073709551616" sh -c 'echo "2^64" | bc'
check "bc -l" "3.14159265358979323844" sh -c 'echo "scale=20; 4*a(1)" | bc -l'
rm -f "$T/k"
check dropbearkey "ssh-ed25519 " sh -c "dropbearkey -t ed25519 -f '$T/k' >/dev/null && dropbearkey -y -f '$T/k'"
check "dropbearkey ecdsa" "ecdsa-sha2-nistp256 " sh -c "dropbearkey -t ecdsa -s 256 -f '$T/k2' >/dev/null && dropbearkey -y -f '$T/k2'"
check nano "GNU nano" nano --version
check less "less " less --version
check ncdu "ncdu " ncdu -v
check htop "htop " htop --version
check lsz "lsz" lsz --version
check tput "80" tput -T xterm cols
check mandel "@" mandel 30
check bogomips "BogoMIPS" bogomips
check "claude-guest-mcp" '"text":"mcp-shell-ok' sh -c "printf '%s\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"bash\",\"arguments\":{\"command\":\"echo mcp-shell-ok\"}}}' | claude-guest-mcp '$T'"

# tmux: a server with a shell in a pty, a command sent to it, its screen read back
S=$T/tmux.sock
export TERM=${TERM:-xterm}
if tmux -S "$S" -f /dev/null new-session -d -s t -x 80 -y 24 "sh" 2>"$T/tmux.err"; then
  tmux -S "$S" send-keys -t t 'echo tmux-$((6*7))' Enter
  sleep 2
  out=$(tmux -S "$S" capture-pane -p -t t 2>&1)
  case $out in *tmux-42*) ok "tmux new-session, send-keys, capture-pane";; *) bad tmux "$out";; esac
  check "tmux list-sessions" "t: 1 windows" tmux -S "$S" list-sessions
  tmux -S "$S" kill-server
else
  bad "tmux new-session" "$(cat "$T/tmux.err")"
fi

if [ "$1" = ssh ]; then
  dropbearkey -t ed25519 -f "$T/hostkey" >/dev/null 2>&1
  dropbearkey -t ed25519 -f "$T/ck" >/dev/null 2>&1
  mkdir -p -m 700 "$HOME/.ssh"
  ak=$HOME/.ssh/authorized_keys
  [ -f "$ak" ] && cp -p "$ak" "$T/ak.saved"
  dropbearkey -y -f "$T/ck" | grep '^ssh-' >> "$ak"
  chmod 600 "$ak"
  dropbear -r "$T/hostkey" -p 127.0.0.1:2222 -P "$T/db.pid" -s 2>"$T/db.err"
  sleep 2
  check "dropbear + dbclient login" "ssh-ok-$(id -u)" dbclient -y -i "$T/ck" -p 2222 "$(id -un)@127.0.0.1" 'echo ssh-ok-$(id -u)'
  # an interactive login needs a terminal on the client side too: tmux's
  tmux -S "$S" -f /dev/null new-session -d -s s -x 80 -y 24 \
    "dbclient -y -t -i '$T/ck' -p 2222 $(id -un)@127.0.0.1 'tty && echo pty-\$((40+2)); sleep 5'"
  sleep 4
  out=$(tmux -S "$S" capture-pane -p -t s 2>&1)
  case $out in *pts*pty-42*) ok "dbclient login with a pty (in tmux)";; *) bad "dbclient with a pty" "$out";; esac
  tmux -S "$S" kill-server 2>/dev/null
  [ -f "$T/db.pid" ] && kill "$(cat "$T/db.pid")"
  if [ -f "$T/ak.saved" ]; then cp -p "$T/ak.saved" "$ak"; else rm -f "$ak"; fi
fi

rm -rf "$T"
echo "run-tests: $([ $fail = 0 ] && echo PASS || echo FAIL) ($pass passed, $fail failed)"
[ $fail = 0 ]
