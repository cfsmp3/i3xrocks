#!/bin/sh
# Regression test: block output longer than BUFSIZ (8 KiB) or than a pipe
# (64 KiB) must reach the bar whole, without hiding the block, its later lines
# or its neighbours, and without stopping it from running again. A persistent
# block shows complete lines only. A command writing endlessly is cut off with
# an error instead of growing the bar's memory. Needs no X server.

I3XROCKS=${I3XROCKS:-./i3xrocks}

command -v timeout >/dev/null 2>&1 || { echo "SKIP: timeout not found"; exit 77; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() {
	echo "FAIL: $*"
	exit 1
}

# Print $2 copies of the character $1
repeat() {
	head -c "$2" /dev/zero | tr '\0' "$1"
}

cat >"$tmp/long.sh" <<'EOF'
head -c 9000 /dev/zero | tr '\0' A
echo
EOF

cat >"$tmp/lines.sh" <<'EOF'
head -c 9000 /dev/zero | tr '\0' B
printf '\nshort\n#00FF00\n'
EOF

cat >"$tmp/huge.sh" <<'EOF'
head -c 1572864 /dev/zero | tr '\0' C
echo
EOF

cat >"$tmp/json.sh" <<'EOF'
awk 'BEGIN { printf "{\"full_text\":\""; for (i = 0; i < 9000; i++) printf "\\\""; print "\"}" }'
EOF

cat >"$tmp/stream.sh" <<'EOF'
echo first
printf 'part-one-'
sleep 1
echo part-two
EOF

cat >"$tmp/counter.sh" <<'EOF'
n=$(($(cat "$1" 2>/dev/null || echo 0) + 1))
echo "$n" >"$1"
printf '%s:' "$n"
head -c 102400 /dev/zero | tr '\0' D
echo
EOF

cat >"$tmp/conf" <<EOF
[left]
command=echo left
interval=once

[long]
command=sh $tmp/long.sh
interval=once

[lines]
command=sh $tmp/lines.sh
interval=once

[huge]
command=sh $tmp/huge.sh
interval=once

[json]
command=sh $tmp/json.sh
format=json
interval=once

[stream]
command=sh $tmp/stream.sh
interval=persist

[big]
command=sh $tmp/counter.sh $tmp/counter
interval=1

[endless]
command=timeout 3 yes
interval=once

[right]
command=echo right
interval=once
EOF

timeout -k 2 5 "$I3XROCKS" -c "$tmp/conf" </dev/null >"$tmp/out" 2>"$tmp/err"
rc=$?
sort -u "$tmp/err" | head -c 2000

# 124: still running when timeout stopped it.
[ "$rc" -eq 124 ] || fail "i3xrocks exited with $rc"

grep '^,\[' "$tmp/out" | tail -n 1 >"$tmp/last"
[ -s "$tmp/last" ] || fail "no status line printed"

names=$(grep -o '"name":"[a-z]*"' "$tmp/last" | tr -d '\n')
[ "$names" = '"name":"left""name":"long""name":"lines""name":"huge""name":"json""name":"stream""name":"big""name":"right"' ] ||
	fail "blocks missing or out of order: $names"

has() {
	printf '%s' "$1" >"$tmp/pattern"
	grep -qF -f "$tmp/pattern" "$2"
}

has "\"full_text\":\"$(repeat A 9000)\"" "$tmp/last" ||
	fail "9000-byte line not shown"
has "\"full_text\":\"$(repeat B 9000)\",\"short_text\":\"short\",\"color\":\"#00FF00\"" "$tmp/last" ||
	fail "lines after a 9000-byte line lost"
has "\"full_text\":\"$(repeat C 1572864)\"" "$tmp/last" ||
	fail "1.5 MiB line not shown"
has "\"full_text\":\"$(awk 'BEGIN { for (i = 0; i < 9000; i++) printf "\\\"" }')\"" "$tmp/last" ||
	fail "9000 escaped quotes from a JSON block not shown"

grep -qF '"full_text":"first"' "$tmp/out" ||
	fail "first persistent line not shown"
grep -qF '"full_text":"part-one-"' "$tmp/out" &&
	fail "partial persistent line shown"
grep -qF '"full_text":"part-one-part-two"' "$tmp/last" ||
	fail "persistent line written in two parts not shown"

# The 100 KiB block runs every second and shows each run in full
runs=$(grep -o '"full_text":"[0-9]*:D*"' "$tmp/out" | sort -u |
	awk '{ s = $0; sub(/^"full_text":"[0-9]*:/, "", s); sub(/"$/, "", s);
	       if (length(s) == 102400) n++ } END { print n + 0 }')
[ "$runs" -ge 2 ] || fail "100 KiB block shown $runs times, expected at least 2"

# Errors reach the bar only with -v; the log always has them
grep -qF "[endless] Command 'timeout 3 yes' wrote over 16 MiB without a status" "$tmp/err" ||
	fail "endless output not reported"

echo "PASS"
