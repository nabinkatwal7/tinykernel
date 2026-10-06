#!/bin/sh
# usage: tools/batch.sh <short-name>
# Starts the next batch of work as a new branch on top of the current one (batches are stacked, so
# each pull request only shows its own commits when its base is the previous batch).
set -e
cd "$(dirname "$0")/.."
prev=$(git branch --show-current)
new="batch/$1"
git checkout -q -b "$new"
printf '%s\n' "$prev" > .git/batch_base_$(printf '%s' "$new" | tr '/' '_')
echo "on $new (stacked on $prev)"
