#!/bin/bash

CURRENT_DIR=$(pwd)
echo "run this bash script  to run all programs in rvv-examples with selected values  of L1D L2, VLEN and DLEN"
echo "each m5out will be renamed to reflect the parameters"

echo "Current Dir is $(pwd)"

echo "----------------------------------------------------------------------------------"
echo "RVV Examples Benchmark Suite"
echo "----------------------------------------------------------------------------------"
echo ""

# Define arrays for programs and options

programs=( "s000"      "s111"      "s1111"      "s112"      "s1112"      "s113"      "s1113"      "s114"      "s115"      "s1115"      "s116"      "s118"      "s119"      "s1119"      "s121"      "s122"      "s123"      "s124"      "s125"      "s126"      "s127"      "s128"      "s131"      "s132"      "s141"      "s151"      "s152"      "s161"      "s1161"      "s162"      "s171"      "s172"      "s173"      "s174"      "s175"      "s176"      "s211"      "s212"      "s1213"      "s221"      "s1221"      "s222"      "s231"      "s232"      "s1232"      "s233"      "s2233"      "s235"      "s241"      "s242"      "s243"      "s244"      "s1244"      "s2244"      "s251"      "s1251"      "s2251"      "s3251"      "s252"      "s253"      "s254"      "s255"      "s256"      "s257"      "s258"      "s261"      "s271"      "s272"      "s273"      "s274"      "s275"      "s2275"      "s276"      "s277"      "s278"      "s279"      "s1279"      "s2710"      "s2711"      "s2712"      "s281"      "s1281"      "s291"      "s292"      "s293"      "s2101"      "s2102"      "s2111"      "s311"      "s31111"      "s312"      "s313"      "s314"      "s315"      "s316"      "s317"      "s318"      "s319"      "s3110"      "s13110"      "s3111"      "s3112"      "s3113"      "s321"      "s322"      "s323"      "s331"      "s332"      "s341"      "s342"      "s343"      "s351"      "s1351"      "s352"      "s353"      "s421"      "s1421"      "s422"      "s423"      "s424"      "s431"      "s441"      "s442"      "s443"      "s451"      "s452"      "s453"      "s471"      "s481"      "s482"      "s491"      "s4112"      "s4113"      "s4114"      "s4115"      "s4116"      "s4117"      "s4121"      "va"      "vag"      "vas"      "vif"      "vpv"      "vtv"      "vpvtv"      "vpvts"      "vpvpv"      "vtvtv"      "vsumr"      "vdotr"      "vbor"  )

compiler=( "clang" "GNU")
executable=( "vec" "novec" )

L1options=("16KiB" "32KiB" "64KiB")
L2options=("256KiB" "512KiB" "1MiB")
VLenoptions=(128 256 512)
#ELenoptions=(16 32 64)
ELenoptions=(64)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sim="/gem5/build/RISCV/gem5.opt"
configscript="${SCRIPT_DIR}/riscv-rvv-se-ara.py"

# Loop through each compiler
for comp in "${compiler[@]}"; do
echo "Processing compiler: $comp"
# Build once per compiler (VLEN is a gem5 runtime option, not a compile-time one)
(cd "${SCRIPT_DIR}/src" && make COMPILER=$comp)
# Loop through each variant
for var in "${executable[@]}"; do
echo "Processing variant: $var"
exec="${SCRIPT_DIR}/bin/${comp}/tsvc_${var}"
if [[ ! -f "$exec" ]]; then
    echo "WARNING: binary $exec not found (build failed or compiler unavailable), skipping variant $var for $comp"
    continue
fi
# Loop through Vlen
for opt3 in "${VLenoptions[@]}"; do
echo "Processing vlen: $opt3"
# Loop through each program
for prog in "${programs[@]}"; do
echo "Processing program: $prog"
    # Loop through each option
    for opt1 in "${L1options[@]}"; do
        echo "Processing L1: $opt1"
	for opt2 in "${L2options[@]}"; do
            echo "Processing L2: $opt2"
		for opt4 in "${ELenoptions[@]}"; do
		    echo "  Running ${prog} with L1D, L2, vlen, elen options: $opt1, $opt2, $opt3, $opt4"
		    trap 'echo keyboard interrupt; exit' INT
		    # Run command with current program and option
                    outdir="TSVC-Gem5-LoopOnly-${comp}-${var}-output/${prog}_${opt1}_${opt2}_${opt3}_${opt4}_m5out"
                    mkdir -p $outdir
                    crashlog="${outdir}/crashed_progs.txt"
                    # Run the sim command; if it exits non-zero, delete the outdir
                    if ! "$sim" -d "$outdir" "$configscript" \
                            --cpu-type AraO3 \
                            --enable-chaining \
                            --simd-units 2 \
                            --vector-timing-throughput 4 \
                            -d "$opt1" -2 "$opt2" -v "$opt3" -e "$opt4" \
                            "$exec" -p "$prog"; then
                      echo "$outdir" >> "$crashlog"
                      echo "Simulation of $prog crashed—logged to $crashlog and deleting $outdir"
                      rm -rf -- "$outdir"
                    else
                      echo "Simulation completed successfully."
                    fi
		done
	done
    done
    echo "--------------PROG-----------------"
done
    echo "--------------VLEN-----------------"
done
    echo "--------------VAR-----------------"
done
    echo "--------------COMP-----------------"
done

echo "All run completed."

