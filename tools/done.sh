#!/bin/sh
# usage: tools/done.sh "<exact inst.txt line without trailing comma>" "commit subject"
# Removes the finished item from inst.txt, commits everything, pushes.
set -e
cd "$(dirname "$0")/.."
grep -vxF "$1," inst.txt > inst.tmp || true
mv inst.tmp inst.txt
git add -A
git commit -q -m "$2

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
git push -q origin HEAD
git log --oneline | head -1
