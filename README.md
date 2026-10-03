# dbox

A folder sync daemon for Linux and macOS. It keeps a local folder in sync
with S3-compatible buckets or plain directories. One store is the primary and
any number are mirrors, so moving to a new backend means adding it as a
mirror, waiting for the backfill and promoting it.

Read [POC.md](POC.md) for the design, the sync rules and what has been verified.

## Quick start

```sh
mise install && make build
cp dev.config.yml ~/.config/dbox/config.yml     # then edit stores and sync.root
./dbox check minio-a                            # probe each store
./dbox run --once                               # first sync, then exit
./dbox service enable                           # keep it running from login
./dbox status
```

## Migrating to another store

```sh
# 1. add the new store with role: mirror, then reload the daemon
kill -HUP "$(cat ~/dbox/.dbox/daemon.pid)"
# 2. wait until status shows it at 100%
./dbox status
# 3. make it the primary; the old primary becomes a mirror
./dbox promote r2
# 4. later, set the old store to role: detached, or remove it
```

## Development

```sh
make minio         # two MinIOs on :9200 and :9300 with a dbox bucket each
make test          # vet and unit tests, no network
make integration   # S3 store tests against the MinIOs
make run           # daemon on dev.config.yml, syncing ./tmp/box
```
