#!/bin/sh
set -eu
exec "$(dirname "$0")/build.sh" Release
