#!/usr/bin/env python3
"""Configure, build and run the transport and depot fuzzers (ADR-105).

The fuzzers live in tests/fuzz and build in their own tree (default
build_fuzz) with VKR_BUILD_FUZZERS, ASan and UBSan. Each fuzzer writes its
seed corpus from code into <build>/fuzz/corpus/<target>, then runs one at a
time for a bounded time with a bounded RSS. A crash leaves its input in
<build>/fuzz/artifacts/<target>/ and makes the script exit nonzero; the
libFuzzer log of every run is kept in <build>/fuzz/logs/.

The default compiler is $CC or the system clang. Apple clang has no
libFuzzer of its own, so cmake/vkr_fuzzers.cmake links the libFuzzer archive
of a Homebrew LLVM install to Apple's ASan and UBSan; Homebrew LLVM 20's own
ASan runtime deadlocks at start-up on macOS 26. Without any libFuzzer each
fuzzer builds as a replay driver that only runs given files (--replay).

Examples:
  python3 tools/checks/run_fuzzers.py --time 120
  python3 tools/checks/run_fuzzers.py --targets vkr_fuzz_depot --time 30
  python3 tools/checks/run_fuzzers.py --no-build --targets vkr_fuzz_depot \\
      --replay tests/fuzz/crashes/depot_tree_case_fold
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# Largest input per target: room for a 1,200-byte Initial and records, or
# a full depot batch message.
TARGETS = {
    "vkr_fuzz_net_core": 4096,
    "vkr_fuzz_net_session": 2048,
    "vkr_fuzz_wire": 2048,
    "vkr_fuzz_depot": 12288,
}

def default_compiler():
    return os.environ.get("CC") or shutil.which("clang") or "clang"


def run(command, **kwargs):
    print("$ " + " ".join(str(part) for part in command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def configure(build, compiler):
    cxx = compiler.replace("clang", "clang++") if compiler.endswith(
        "clang") else compiler
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    run(["cmake", "-S", ROOT, "-B", build, *generator,
         f"-DCMAKE_C_COMPILER={compiler}", f"-DCMAKE_CXX_COMPILER={cxx}",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DVKR_BUILD_FUZZERS=ON",
         "-DVKR_BUILD_RUNTIME=OFF", "-DVKR_BUILD_HARNESS=OFF",
         "-DVKR_BUILD_TESTS=OFF", "-DVKR_BUILD_APP=OFF",
         "-DVKR_BUILD_PLAYER=OFF", "-DVKR_BUILD_EDITOR=OFF",
         "-DVKR_BUILD_TOOLS=ON", "-DVKR_ENABLE_SANITIZERS=OFF",
         "-DVKR_ENABLE_DEBUG_SANITIZERS=OFF"])


def engine(build):
    cache = (build / "CMakeCache.txt").read_text(errors="replace")
    native = re.search(r"^VKR_FUZZ_HAVE_LIBFUZZER:INTERNAL=1$", cache,
                       re.MULTILINE)
    archive = re.search(r"^VKR_FUZZ_LIBFUZZER_LIBRARY:FILEPATH=(\S+)", cache,
                        re.MULTILINE)
    return "libfuzzer" if native or archive else "replay"


def sanitizer_environment():
    env = dict(os.environ)
    # Apple's ASan has no LeakSanitizer; the fuzzers count their own bytes.
    env.setdefault("ASAN_OPTIONS", "abort_on_error=0")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
    return env


def summary(log_text):
    """The last coverage line and the executions libFuzzer reported."""
    coverage = None
    for line in log_text.splitlines():
        if re.match(r"#\d+\s+(DONE|pulse|NEW|REDUCE|INITED)", line):
            coverage = line.strip()
    runs = re.search(r"stat::number_of_executed_units:\s*(\d+)", log_text)
    return coverage, runs.group(1) if runs else "?"


def fuzz(build, target, seconds, rss_mb):
    exe = build / "tests" / "fuzz" / target
    work = build / "fuzz"
    corpus = work / "corpus" / target
    artifacts = work / "artifacts" / target
    logs = work / "logs"
    for directory in (corpus, artifacts, logs):
        directory.mkdir(parents=True, exist_ok=True)
    env = sanitizer_environment()
    run([exe], env=dict(env, VKR_FUZZ_SEED_DIR=str(corpus)), timeout=120)
    if not any(corpus.iterdir()):
        sys.exit(f"{target} wrote no seeds; LLVMFuzzerInitialize did not run")
    log = logs / f"{target}.log"
    command = [exe, corpus, f"-max_total_time={seconds}",
               f"-rss_limit_mb={rss_mb}", f"-max_len={TARGETS[target]}",
               "-timeout=20", "-print_final_stats=1",
               f"-artifact_prefix={artifacts}/"]
    print("$ " + " ".join(str(part) for part in command), flush=True)
    with open(log, "w") as out:
        result = subprocess.run(command, env=env, stdout=out,
                                stderr=subprocess.STDOUT)
    text = log.read_text(errors="replace")
    coverage, runs = summary(text)
    crashes = sorted(p.name for p in artifacts.iterdir())
    print(f"{target}: exit {result.returncode}, {runs} executions, "
          f"{coverage or 'no coverage line'}; log {log}")
    if result.returncode != 0 or crashes:
        print(f"{target}: FAILED; artifacts: {crashes}")
        tail = text.splitlines()[-60:]
        print("\n".join(tail))
        return False
    return True


def replay(build, target, files):
    exe = build / "tests" / "fuzz" / target
    result = subprocess.run([exe, *files], env=sanitizer_environment())
    print(f"{target}: replay of {len(files)} files exited "
          f"{result.returncode}")
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default="build_fuzz")
    parser.add_argument("--cc", default=default_compiler(),
                        help="the Clang to build with (default: $CC or "
                             "clang on PATH)")
    parser.add_argument("--targets", nargs="+", choices=sorted(TARGETS),
                        default=list(TARGETS))
    parser.add_argument("--time", type=int, default=60,
                        help="seconds of fuzzing per target")
    parser.add_argument("--rss-limit-mb", type=int, default=2048)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--reconfigure", action="store_true")
    parser.add_argument("--replay", nargs="+", metavar="FILE",
                        help="run these inputs once instead of fuzzing")
    args = parser.parse_args()

    build = pathlib.Path(args.build_dir)
    if not build.is_absolute():
        build = ROOT / build
    if not args.no_build:
        if args.reconfigure or not (build / "CMakeCache.txt").exists():
            configure(build, args.cc)
        run(["cmake", "--build", build, "--target", *args.targets])

    ok = True
    if args.replay:
        for target in args.targets:
            ok = replay(build, target, args.replay) and ok
        return 0 if ok else 1
    if engine(build) != "libfuzzer":
        sys.exit("this tree has no libFuzzer; use --replay or configure "
                 "with a Clang that ships it (--cc, --reconfigure)")
    # One fuzzer process at a time keeps memory bounded.
    for target in args.targets:
        ok = fuzz(build, target, args.time, args.rss_limit_mb) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
