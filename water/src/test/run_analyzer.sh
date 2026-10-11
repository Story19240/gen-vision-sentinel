#!/bin/bash
cd /data/workspace/water/src/test
make -j4
if [ $? -eq 0 ]; then
    unset DISPLAY
    ./water_flow_analyzer
fi
