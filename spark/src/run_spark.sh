#!/bin/bash
set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
cd "$ROOT_DIR"
make -C spark/src monitor
exec spark/src/bin/spark_monitor_server --config spark/spark_config.json "$@"
