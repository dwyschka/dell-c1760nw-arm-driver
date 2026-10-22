#!/bin/sh
# run-tests.sh - regression tests of rastertoxrhbpl against reference hashes
# that were produced with the original Xerox filters.
#
#   sh test/run-tests.sh                 check the port (any platform)
#   REF=/path/to/XRM_dir sh test/run-tests.sh > test/expected.txt
#                                        regenerate references (macOS + Rosetta)
#
# Needs the DLUT and PPD of the original driver: set XEROX_PAYLOAD to the
# Payload directory of the macOS driver package (tests needing it are
# skipped otherwise).
set -u
cd "$(dirname "$0")/.."
T=${TMPDIR:-/tmp}/xrtest.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT

CC=${CC:-cc}
$CC -O2 -o "$T/gen" test/gen.c || exit 1
$CC -O2 -Wno-deprecated-declarations -o "$T/gencups" test/gencups.c -lcups || exit 1

if command -v sha256sum >/dev/null; then sha() { sha256sum | cut -c1-64; }
else sha() { shasum -a 256 | cut -c1-64; }; fi

PAYLOAD=${XEROX_PAYLOAD:-}
DLUT="$PAYLOAD/Library/Printers/Xerox/DLUT/Xerox Phaser 6010.dlut"
ORIGPPD="$PAYLOAD/Library/Printers/PPDs/Contents/Resources/Xerox Phaser 6000B.gz"
have_payload=0
if [ -n "$PAYLOAD" ] && [ -f "$DLUT" ] && [ -f "$ORIGPPD" ]; then
    have_payload=1
    sh tools/make-ppd.sh "$ORIGPPD" "$DLUT" > "$T/test.ppd"
    if [ -n "${REF:-}" ]; then
        # the original filters read their chain and DLUT location from the PPD
        gzip -dc "$ORIGPPD" | sed -e "s#/Library/Printers/Xerox/filter/XeroxHBFilter.bundle/Contents/MacOS#$REF#g" \
            -e "s#/Library/Printers/Xerox/DLUT/Xerox Phaser 6010.dlut#$DLUT#" > "$T/test.ppd"
    fi
    export PPD="$T/test.ppd"
fi

# normalise host dependent PJL lines
norm() { LC_ALL=C sed -E 's/(DATE|TIME)=[0-9:\/]+/\1=X/; s/@(HOAD|CNAM)=[^"]*/@\1=X/'; }

run_case() { # prints the hash of one case
    kind=$1; shift
    case $kind in
    al5)
        "$T/gen" $1 $2 $3 $4 $5 > "$T/in"
        if [ -n "${REF:-}" ]; then
            opt=""; [ "$1" = mono ] && opt="XRColorMode=Black"
            arch -x86_64 "$REF/XRM_AL5CB" 1 user title 1 "$opt" < "$T/in" | sha
        else
            flag=""; [ "$1" = mono ] && flag="--mono"
            test/al5filter $flag < "$T/in" | sha
        fi ;;
    cc)
        "$T/gen" rgb $1 $2 $3 $4 > "$T/in"
        if [ -n "${REF:-}" ]; then
            opt="XRDocumentType=$5"; [ "$6" = 1 ] && opt="$opt XRColorMode=Black"
            arch -x86_64 "$REF/XRM_CC" 1 user title 1 "$opt" < "$T/in" | sha
        else
            test/ccfilter "$DLUT" "$5" "$6" < "$T/in" | sha
        fi ;;
    job)
        "$T/gencups" $1 $2 $3 $4 > "$T/in"
        copies=$5; shift 5; opts="$*"
        if [ -n "${REF:-}" ]; then
            arch -x86_64 "$REF/XRM_MF" 1 user title "$copies" "$opts" "$T/in" 2>/dev/null | norm | sha
        else
            ./rastertoxrhbpl 1 user title "$copies" "$opts" "$T/in" 2>/dev/null | norm | sha
        fi ;;
    esac
}

pass=0 fail=0 skip=0
cases=test/expected.txt
[ -n "${REF:-}" ] && cases=test/cases.txt
while IFS='|' read -r name args expected; do
    case $name in ''|'#'*) continue ;; esac
    case $name in
    cc*|job*) if [ $have_payload = 0 ]; then skip=$((skip + 1)); continue; fi ;;
    esac
    # shellcheck disable=SC2086
    got=$(run_case $args)
    if [ -n "${REF:-}" ]; then
        echo "$name|$args|$got"
    elif [ "$got" = "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name ($args)" >&2
    fi
done < "$cases"

if [ -z "${REF:-}" ]; then
    echo "passed: $pass  failed: $fail  skipped: $skip (set XEROX_PAYLOAD to run all)"
    [ $fail = 0 ]
fi
