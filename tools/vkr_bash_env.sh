#!/usr/bin/env bash
#
# vkr_bash_env.sh
#
# This file is sourced via BASH_ENV for non-interactive bash invocations
# launched by CMake/Ninja on Windows.
#
# Why it exists:
# - KTX's scripts/mkversion starts with `#!/usr/bin/env bash`.
# - When mkversion is executed as a program, `/usr/bin/env` searches PATH
#   for `bash`. If PATH begins with Windows entries, it may resolve to
#   `C:\Windows\System32\bash.exe` (the WSL stub), causing the "install WSL"
#   message and build failure.
#
# Fix:
# - Force POSIX toolchain paths first so `env bash` resolves to the MSYS/Git
#   bash that was selected for the build.
#

case ":$PATH:" in
  *":/usr/bin:"*) ;;
  *) export PATH="/usr/bin:/bin:$PATH" ;;
esac

