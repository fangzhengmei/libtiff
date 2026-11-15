#!/usr/bin/env bash
# Build the harness against the ASan/UBSan libtiff and run all samples.
set -u

ROOT=/srv/fb2/fangzhengmei/0062-libtiff-10-6b469d15
BUILD=$ROOT/asan-build
WORK=$ROOT/corrupt-ifd-work
SAMPLES=$WORK/samples
RESULTS=$WORK/results
mkdir -p "$RESULTS"

# Link the instrumented shared library with an rpath so no env vars are needed.
cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
   -I"$BUILD/libtiff" -I"$ROOT/libtiff" \
   "$WORK/scripts/harness.c" \
   -L"$BUILD/libtiff" -ltiff \
   -Wl,-rpath,"$BUILD/libtiff" \
   -o "$WORK/scripts/harness" -lm
echo "harness build exit=$?"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1:print_stacktrace=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

LOG="$RESULTS/harness_run.log"
: > "$LOG"
"$WORK/scripts/harness" "$LOG" "$SAMPLES"/*.tif
echo "harness exit=$?"

# Second witness: the instrumented tiffinfo (mmap path; the harness above
# covers both mmap and stream). -d decodes image data too.
TI="$BUILD/tools/tiffinfo"
for f in "$SAMPLES"/*.tif; do
  name=$(basename "$f")
  {
    echo "==== tiffinfo -d $name ===="
    "$TI" -d "$f" 2>&1
    echo "exit=$?"
  } >> "$RESULTS/tiffinfo_run.log"
done
echo "done; logs in $RESULTS"
