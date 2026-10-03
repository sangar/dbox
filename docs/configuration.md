# Configuration

The config file is `~/.config/dbox/config.yml` on Linux and macOS
(`$XDG_CONFIG_HOME` is respected). `--config FILE` or `DBOX_CONFIG` points at
another file, and an existing `config.yaml` is used when there is no
`config.yml`. `dbox config edit` opens it in your editor, checks it and
reloads a running daemon.

Secrets can also come from the standard AWS environment variables or `~/.aws/credentials`, which the SDK picks up by
default when `access_key`/`secret_key` are omitted on a store. Any value can
be `${ENV_VAR}` and is expanded at load, so secrets stay out of the file;
references in comments are left alone.

## Full example

```yaml
# ~/.config/dbox/config.yml

stores:
  minio:                      # name; stable, recorded in the index
    kind: s3
    role: primary             # primary | mirror | detached
    bucket: dbox
    prefix: gard/laptop/      # optional; keys are prefix + relative path
    region: us-east-1
    endpoint: http://localhost:9200
    path_style: true
    access_key: minioadmin
    secret_key: ${MINIO_SECRET}
    workers: 4

  r2:
    kind: s3
    role: mirror
    bucket: dbox-gard
    region: auto
    endpoint: https://<account>.eu.r2.cloudflarestorage.com
    access_key: ${R2_ACCESS_KEY}
    secret_key: ${R2_SECRET_KEY}
    workers: 2

  aws:
    kind: s3
    role: detached            # known to the daemon, receives nothing, objects stay readable
    bucket: dbox-archive
    region: eu-north-1
    storage_class: STANDARD_IA
    # no keys: SDK default credential chain

  nas:
    kind: disk
    role: mirror
    root: /Volumes/backup/dbox
    workers: 1

sync:
  root: ~/dbox
  pull_interval: 30s          # poll the primary
  backfill_interval: 10m      # sweep mirrors for missing replicas
  debounce: 750ms
  part_size: 8MiB
  delete_remote: true         # false = never delete in any store, only upload
  delete_local: true          # false = never delete on disk from a poll
  ignore:
    - .git/
    - .DS_Store
    - "*.swp"
    - "*.tmp"
    - "~$*"
    - .dbox/

daemon:
  log_level: info             # debug | info | warn | error
  log_format: text            # text | json
  listen: 127.0.0.1:7878      # /healthz and /metrics; empty disables
```

## Validation

At load: exactly one `primary`, names match `[a-z0-9_-]+`, `s3`
stores have bucket and region, `disk` stores have an existing root. A config
with zero stores is accepted and the daemon idles with a warning, mirroring
immish's "no primary yet" state. Unknown keys are an error.

## Environment overrides

Environment overrides use the `DBOX_` prefix with `__` for nesting, e.g.
`DBOX_SYNC__ROOT`. Store secrets use `${VAR}` in the file instead.
