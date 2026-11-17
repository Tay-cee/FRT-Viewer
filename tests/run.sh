#!/bin/sh
set -eu
FRT=${1:-$(dirname "$0")/../frt}
case $FRT in /*) ;; *) FRT=$PWD/$FRT ;; esac
cd "$(dirname "$0")"
OUT=out
rm -rf "$OUT" && mkdir -p "$OUT"
fail() { echo "FAIL: $*" >&2; exit 1; }

"$FRT" parse fixture.txt > "$OUT/parsed.txt" 2>/dev/null
diff -u expected.txt "$OUT/parsed.txt" || fail "parser output changed (update tests/expected.txt if intended)"
echo "ok   parse"

awk '{ print }
     /^ *900002 +Acme Test Arms +T-1 Pistol/ { printf "%85s%-49s%s\n", "", "1911 A1", "Division" }
     /^ *900001 +- +1 / { printf "%33s%s\n", "", "45 - 70 GOVT" }' fixture.txt > "$OUT/wrapped.txt"
"$FRT" parse "$OUT/wrapped.txt" 2>/dev/null > "$OUT/wrapped-parsed.txt"
grep -q "XREF 900002 | Acme Test Arms | T-1 Pistol 1911 A1 | Acme Test Arms Ltd Division |" "$OUT/wrapped-parsed.txt" ||
  fail "wrapped cross-ref line starting with a number"
grep -q "CAL 1 | 5.56MM NATO 45 - 70 GOVT |" "$OUT/wrapped-parsed.txt" || fail "wrapped calibre line starting with a number"
[ "$(grep -c '^  XREF' "$OUT/wrapped-parsed.txt")" = 3 ] || fail "phantom cross-ref row"
[ "$(grep -c '^  CAL' "$OUT/wrapped-parsed.txt")" = "$(grep -c '^  CAL' expected.txt)" ] || fail "phantom calibre row"
echo "ok   wrapped rows starting with digits"

DB="$OUT/test.db"
"$FRT" --db "$DB" import fixture.txt | grep -q "2 records (initial import)" || fail "baseline import"
echo "ok   baseline import"

"$FRT" --db "$DB" import fixture.txt | grep -q "already imported" || fail "duplicate not skipped"
echo "ok   duplicate skipped"

sed -e 's#2030/01/15#2030/07/01#' fixture.txt |
  awk 'BEGIN{RS="\f"; ORS="\f"} !/\(FRN\): +900001/' |
  sed -e 's/Legal Classification:\( *\)Prohibited/Legal Classification:\1Restricted/' > "$OUT/v2.txt"
"$FRT" --db "$DB" import "$OUT/v2.txt" | grep -q "modified 1, removed 1" || fail "v2 diff counts"
"$FRT" --db "$DB" changes | grep -q 'legal_classification: "Prohibited" -> "Restricted"' || fail "change log"
"$FRT" --db "$DB" show 900001 | grep -q "REMOVED" || fail "removed flag"
echo "ok   update: modified + removed"

"$FRT" --db "$DB" import fixture.txt | grep -q "newer report" || fail "older report not refused"
sed -e 's#2030/01/15#2031/01/01#' fixture.txt > "$OUT/v3.txt"
"$FRT" --db "$DB" import "$OUT/v3.txt" | grep -q "restored 1" || fail "restore"
echo "ok   older refused, restore"

"$FRT" --db "$DB" search acme t-1 2>/dev/null | grep -q 900002 || fail "search"
echo "ok   search"
echo "all tests passed"
