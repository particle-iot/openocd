#!/bin/sh
#

# Exclude generated source code
pathspec=':!contrib/gperf'
since=${1:-HEAD^}
git format-patch -M --stdout $since -- . $pathspec | tools/scripts/checkpatch.pl - --no-tree
