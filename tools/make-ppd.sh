#!/bin/sh
# make-ppd.sh - derive a Linux PPD for rastertoxrhbpl from the PPD of the
# original Xerox macOS driver ("Xerox Phaser 6000B.gz" / "Xerox Phaser 6010N.gz").
#
# usage: make-ppd.sh ORIGINAL_PPD DLUT_PATH > out.ppd
set -eu
[ $# -eq 2 ] || { echo "usage: $0 ORIGINAL_PPD DLUT_PATH" >&2; exit 1; }
src=$1
dlut=$2

# the original files are gzip compressed but have no .gz suffix
if [ "$(head -c 2 "$src" | od -An -tx1 | tr -d ' \n')" = "1f8b" ]; then
    reader="gzip -dc"
else
    reader="cat"
fi

$reader "$src" | awk -v dlut="$dlut" '
    # host side image filters that are not ported: trapping, edge
    # enhancement, sharpness (the printer gets them switched off)
    /XRTrapping|XREdgeEnhance|XRDigitalFilter/ { next }
    # macOS specific keywords
    /^\*(APPrinterIconPath|APSupportsCustomColorMatching|APDialogExtension|XRFilterDir|XRFilterChain|XRM_[A-Z0-9]+ExecCondition)[ :]/ { next }
    /^\*cupsFilter:/ { print "*cupsFilter: \"application/vnd.cups-raster 0 rastertoxrhbpl\""; next }
    /^\*XRDlutFilePath:/ { print "*XRDlutFilePath: \"" dlut "\""; next }
    /^\*NickName:/ { sub(/"$/, ", rastertoxrhbpl\""); print; next }
    { print }
'
