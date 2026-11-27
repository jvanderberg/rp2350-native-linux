#!/bin/sh
set -eu
unset VIM VIMRUNTIME
if [ "$#" -eq 0 ]; then
    set -- "$(dirname "$0")/vim"
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$@" --version > "$tmp/version"
grep -q 'Tiny version' "$tmp/version"
printf 'before\n' > "$tmp/edit"
"$@" -N -u NONE -n -es -c 'normal! cwafter' -c wq "$tmp/edit"
[ "$(cat "$tmp/edit")" = after ]
printf 'abc123\n' > "$tmp/edit"
"$@" -N -u NONE -n -es -c 's/\([a-z]\+\)\([0-9]\+\)/\2-\1/' -c wq "$tmp/edit"
[ "$(cat "$tmp/edit")" = 123-abc ]

if ! "$@" --clean -n -es -V1 -c 'help quickref' -c 'qall!' > "$tmp/help" 2>&1 ||
   grep -qE 'E[0-9]+:' "$tmp/help" ||
   ! grep -q 'quickref.txt' "$tmp/help"; then
    cat "$tmp/help" >&2
    exit 1
fi
echo "Vim startup, edit, regex substitution, save and help passed"
