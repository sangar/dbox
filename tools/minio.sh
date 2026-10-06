#!/bin/sh
# Starts the two MinIOs from docker-compose.yml, on :9200 and :9300, each with a dbox bucket, for the integration test.
set -e
cd "$(dirname "$0")/.."
docker compose up -d --wait
for s in minio-a minio-b; do
    docker compose exec "$s" sh -c 'mc alias set local http://localhost:9000 minioadmin minioadmin >/dev/null && mc mb -p local/dbox'
done
