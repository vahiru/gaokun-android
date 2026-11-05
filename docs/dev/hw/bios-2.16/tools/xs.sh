#!/bin/bash
S=$(dirname "$0"); f=$1; shift
for s in "$@"; do echo "-- $s"; python3 $S/pe.py "$f" xs "$s"; python3 $S/pe.py "$f" xs "$s" 1; done
