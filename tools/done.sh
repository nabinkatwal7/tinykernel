#!/bin/sh
# usage: tools/done.sh "<exact inst.txt line without trailing comma>" "commit subject" [paths...]
# Removes the finished item from inst.txt, commits (only the given paths, or everything), pushes.
set -e
cd "$(dirname "$0")/.."
item="$1"; msg="$2"; shift 2
tr -d '\r' < inst.txt | grep -vxF "$item," > inst.tmp || true
mv inst.tmp inst.txt
if [ $# -gt 0 ]; then git add inst.txt "$@"; else git add -A; fi
git commit -q -m "$msg

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push -q origin HEAD
git log --oneline | head -1
