#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# test_port_reader.sh: extracts the strict port-table reader of ane-run.c (the one ane-session includes) into a small host program and feeds it port
# tables. A trailing unknown member (a string, a number, an object, an array) after the documented ones must not make the table malformed: before, the skip
# of a scalar value that was the LAST member of the top-level object never stopped at the closing brace, and the M2 session refused a LOAD with bad-ports.
# Cases: a valid table, an unknown scalar and string member in the middle and at the end, an unknown object and array member at the end, a truncated table,
# a table without ports. Needs only cc and libane/ane_m2.h. usage: sh test_port_reader.sh   (run from tools/)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
start=$(grep -n '^struct port_read' "$here/ane-run.c" | head -1 | cut -d: -f1)
end=$(grep -n '^static void free_port_read' "$here/ane-run.c" | head -1 | cut -d: -f1)
# the comment block above the struct belongs to it: back up to the previous blank line
while [ "$start" -gt 1 ] && [ -n "$(sed -n "$((start - 1))p" "$here/ane-run.c" | tr -d ' \t')" ]; do start=$((start - 1)); done
{
  printf '#include <errno.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include "ane.h"\n#include "ane_m2.h"\n'
  sed -n "${start},$((end - 1))p" "$here/ane-run.c"
  cat <<'EOF'
int main(int argc, char **argv)
{
	struct port_read pr;
	int i, rc;

	for (i = 1; i < argc; i++) {
		memset(&pr, 0, sizeof(pr));
		rc = read_port_file(argv[i], &pr);
		printf("%d %u\n", rc, (unsigned)pr.count);
	}
	return 0;
}
EOF
} > "$tmp/h.c"
cc -Wall -O1 -I"$here/../libane" -I"$here/../ane/src/uapi/drm" -o "$tmp/h" "$tmp/h.c" 2>"$tmp/cc.err" || { cat "$tmp/cc.err" >&2; echo "FAIL cannot build the extracted reader" >&2; exit 1; }
port='{"name": "t0", "direction": "input", "shape": [1, 1, 16, 128], "dtype": "fp16", "strides": [4096, 4096, 256, 2], "surface_bytes": 4096, "tile_bytes": 16384, "bar_slot": 4, "buffer_id": 5, "channel": 5}'
mk() { printf '%s\n' "$2" > "$tmp/$1.json"; }
mk ok "{\"program\": \"prog_004\", \"ports\": [$port]}"
mk mid_scalar "{\"program\": \"prog_004\", \"schema_version\": 2, \"ports\": [$port]}"
mk end_string "{\"program\": \"prog_004\", \"ports\": [$port], \"note\": \"a, b (c): d\"}"
mk end_number "{\"program\": \"prog_004\", \"ports\": [$port], \"n\": 7}"
mk end_object "{\"program\": \"prog_004\", \"ports\": [$port], \"o\": {\"a\": [1, 2]}}"
mk end_array "{\"program\": \"prog_004\", \"ports\": [$port], \"a\": [1, 2, 3]}"
mk end_exceptions_then_string "{\"program\": \"prog_004\", \"ports\": [$port], \"exceptions\": [], \"x\": \"y\"}"
mk truncated "{\"program\": \"prog_004\", \"ports\": [$port]"
mk no_ports "{\"program\": \"prog_004\"}"
out=$("$tmp/h" "$tmp/ok.json" "$tmp/mid_scalar.json" "$tmp/end_string.json" "$tmp/end_number.json" "$tmp/end_object.json" "$tmp/end_array.json" "$tmp/end_exceptions_then_string.json" "$tmp/truncated.json" "$tmp/no_ports.json" 2>/dev/null | tr '\n' ' ')
want="0 1 0 1 0 1 0 1 0 1 0 1 0 1 -1 1 -1 0 "
fails=0
if [ "$out" != "$want" ]; then
  echo "FAIL port reader results '$out' want '$want' (order: ok, mid_scalar, end_string, end_number, end_object, end_array, end_exceptions_then_string, truncated, no_ports)" >&2
  fails=1
fi
[ "$fails" = 0 ] && echo "test_port_reader: all checks passed"
exit "$fails"
