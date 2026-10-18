#!/bin/sh
# Builds and runs the unit tests (Linux or MSYS2, gcc). Run from anywhere: sh c/run-unit-tests.sh
cd "$(dirname "$0")" || exit 1
make -s rawstack_test rawhdr_test rawhdr_local_test rawhdr_edge_test rawtile_test rawsr_test fwmem_test rawcmp_test \
        rawsharp_test rawreg_test rawchart_test cleangain_test streakcal_test negconv_test || exit 1
fail=0
t() { name=$1; shift; if "$@" > "/tmp/smart342-$name.out" 2>&1 && grep -q -E "ALL PASS|[0-9]+ checks, 0 failed" "/tmp/smart342-$name.out"; then echo "PASS $name"; else echo "FAIL $name (see /tmp/smart342-$name.out)"; fail=1; fi; }
t rawhdr ./rawhdr_test
t rawhdr_local ./rawhdr_local_test
t rawhdr_edge ./rawhdr_edge_test
t rawtile ./rawtile_test
t rawsr ./rawsr_test
t fwmem ./fwmem_test
t rawsharp ./rawsharp_test
t rawreg ./rawreg_test
t rawchart ./rawchart_test ../ref/chart/s23p1.bin ../ref/chart/s24p1.bin
t cleangain ./cleangain_test ../ref/streak/s34_white12.bin ../ref/streak/vendor_gain605.bin
t streakcal ./streakcal_test ../ref/streak
t negconv ./negconv_test ../ref/neg
t rawcmp sh -c "cd .. && c/rawcmp_test"
t rawstack ./rawstack_test
exit $fail
