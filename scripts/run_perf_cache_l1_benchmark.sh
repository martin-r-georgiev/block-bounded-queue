#!/bin/bash

set -euo pipefail

usage() {
    echo "Usage: $0 <queue> [-p producers] [-c consumers]"
    echo "  <queue>: simple|complex|follyq|boost-lfq|boost-bsyncq|stl"
    echo "  -p N[:max[:step]] (default: 1)"
    echo "  -c N[:max[:step]] (default: 1)"
    exit 1
}

# Validate that at least the queue type is provided
if [ "$#" -lt 1 ]; then
    usage
fi

# Parse colon-delimited range: N[:max[:step]]
parse_range() {
    local arg="$1"
    local min max step
    IFS=':' read -r min max step <<< "$arg"
    min=${min:-1}
    max=${max:-$min}
    step=${step:-1}
    echo "$min $max $step"
}

MODE=$1
shift # Remove the first argument (mode) so that getopts does not process it

# Validate mode
case "$MODE" in
    simple|complex|follyq|boost-lfq|boost-bsyncq|stl)
        ;;
    *)
        echo "Invalid mode provided. Valid modes are: simple, complex, follyq, boost-lfq, boost-bsyncq, stl."
        exit 1
        ;;
esac

PROD_ARG=1
CONS_ARG=1

# Parse optional flags after mode
while getopts ":p:c:" opt; do
    case $opt in
        p) PROD_ARG="$OPTARG" ;;
        c) CONS_ARG="$OPTARG" ;;
        *) usage ;;
    esac
done

read MIN_PRODUCERS MAX_PRODUCERS STEP_PRODUCERS <<< "$(parse_range "$PROD_ARG")"
read MIN_CONSUMERS MAX_CONSUMERS STEP_CONSUMERS <<< "$(parse_range "$CONS_ARG")"

FILENAME_SUFFIX=spsc
if [[ "${MAX_PRODUCERS}" -gt 1 && "${MAX_CONSUMERS}" -gt 1 ]]; then
    FILENAME_SUFFIX=mpmc
elif [[ "${MAX_PRODUCERS}" -gt 1 ]]; then
    FILENAME_SUFFIX=mpsc
elif [[ "${MAX_CONSUMERS}" -gt 1 ]]; then
    FILENAME_SUFFIX=spmc
fi

OUTPUT_FILE="perf_${MODE}_${FILENAME_SUFFIX}_l1.csv"

# Clear the output file if it exists
> "$OUTPUT_FILE"

# Write the headers to the CSV file
echo "producers,consumers,l1_dcache_loads,l1_dcache_load_misses" > "$OUTPUT_FILE"

# Run the benchmark for each producer count
for (( p=MIN_PRODUCERS; p<=MAX_PRODUCERS; p+=STEP_PRODUCERS )); do
    for (( c=MIN_CONSUMERS; c<=MAX_CONSUMERS; c+=STEP_CONSUMERS )); do
        echo "Running benchmark with $p producers and $c consumers..."
        perf stat -x , -e L1-dcache-loads,L1-dcache-load-misses -o temp_perf_output.txt ../bin/benchmark_runner $MODE -i 20 -p $p -c $c --throughput

        L1_LOADS=$(grep "L1-dcache-loads" temp_perf_output.txt | awk -F, '{print $1}')
        L1_MISSES=$(grep "L1-dcache-load-misses" temp_perf_output.txt | awk -F, '{print $1}')

        echo "$p,$c,$L1_LOADS,$L1_MISSES" >> "$OUTPUT_FILE"
    done
done

# Clean up temporary file
rm temp_perf_output.txt

echo "Benchmark results saved to $OUTPUT_FILE"