# dbox

A folder sync daemon for Linux and macOS. It keeps a local folder in sync
with S3-compatible buckets or plain directories. One store is the primary and
any number are mirrors, so moving to a new backend means adding it as a
mirror, waiting for the backfill and promoting it.

Read [POC.md](POC.md) for the design, the sync rules and what has been verified.

## Quick start

```sh
mise install && go install .   # or: make build
dbox config edit               # writes a starter config and opens it in $EDITOR
dbox check minio-a             # probe each store
dbox run --once                # first sync, then exit
dbox service enable            # keep it running from login
dbox status
```

## Configuration

The config lives at `~/.config/dbox/config.yml` (`$XDG_CONFIG_HOME` is
respected). `--config FILE` or `DBOX_CONFIG` points at another file, and an
existing `config.yaml` is used when there is no `config.yml`.

```sh
dbox config          # print the effective config, secrets masked
dbox config --init   # write a commented starter config
dbox config edit     # open it in $VISUAL or $EDITOR, validate, reload the daemon
```

Unknown keys are an error. Any value can be `${ENV_VAR}`, and any key can be
overridden as `DBOX_SECTION__KEY`, e.g. `DBOX_SYNC__PULL_INTERVAL=1m`. Every
key is described in [POC.md](POC.md#configuration).

## Migrating to another store

```sh
# 1. add the new store with role: mirror, then reload the daemon
kill -HUP "$(cat ~/dbox/.dbox/daemon.pid)"
# 2. wait until status shows it at 100%
dbox status
# 3. make it the primary; the old primary becomes a mirror
dbox promote r2
# 4. later, set the old store to role: detached, or remove it
```

## Development

```sh
make minio         # two MinIOs on :9200 and :9300 with a dbox bucket each
make test          # vet and unit tests, no network
make integration   # S3 store tests against the MinIOs
make run           # daemon on dev.config.yml, syncing ./tmp/box
```
