#!/bin/sh
#

since=${1:-HEAD^}
git format-patch --stdout $since | tools/scripts/checkpatch.pl - --no-tree
