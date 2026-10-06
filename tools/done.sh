#!/bin/sh
# usage: tools/done.sh "<exact inst.txt line without trailing comma>" "commit subject" [paths...]
# Removes the finished item from inst.txt and commits it on the current batch branch, then pushes
# that branch. Never touches main: batches are merged through pull requests.
set -e
cd "$(dirname "$0")/.."
branch=$(git branch --show-current)
[ "$branch" != main ] || { echo "refusing to commit on main: run tools/batch.sh <name> first"; exit 1; }
item="$1"; msg="$2"; shift 2
tr -d '\r' < inst.txt | grep -vxF "$item," > inst.tmp || true
mv inst.tmp inst.txt
if [ $# -gt 0 ]; then git add inst.txt "$@"; else git add -A; fi
git commit -q -m "$msg

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push -q -u origin "$branch" 2>/dev/null
git log --oneline | head -1
