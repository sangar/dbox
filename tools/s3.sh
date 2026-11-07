#!/bin/sh
# Starts the two SeaweedFS instances from docker-compose.yml, on :9200 and :9300, each with a dbox bucket, for the integration test.
set -e
cd "$(dirname "$0")/.."
docker compose up -d --wait
