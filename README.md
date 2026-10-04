# dbox

Keep a local folder in sync with S3-compatible buckets or plain directories,
sync-folder style: files live on disk and work offline, local changes are pushed
within a second, and changes made elsewhere are pulled every 30 seconds.

One store is the **primary** and any number are **mirrors**. Every file goes
to the primary first and is then copied to each mirror, so moving to a new
backend means adding it as a mirror, waiting for the copy, and promoting it.
dbox runs on Linux and macOS as a single binary, with no root access and no
kernel extensions.

> **Status: proof of concept.** Sync, mirroring, promotion and the login
> service work and are tested against MinIO and plain directories, but dbox
> has not been run against real-world data for long. Keep another backup.

## How it syncs

- **Local changes** (create, edit, rename, delete) are uploaded to the
  primary, then copied from the primary to each mirror and verified.
- **Remote changes** are pulled from the primary only. Mirrors are never read
  from, except to check which copies they already hold.
- **Deletes travel both ways** by default: deleting a file locally deletes it
  in every store, and a file deleted in the primary is deleted locally. Set
  `delete_remote: false` or `delete_local: false` to turn either off.
- **Conflicts:** when a file changed both locally and in the primary, the
  local copy wins and the remote one is saved next to it as
  `name.conflict-<host>-<timestamp>`.
- **After downtime**, dbox compares the folder, its index and the primary on
  startup, so nothing changed while it was stopped is missed.

State lives in `<folder>/.dbox/`, which is never synced.

## Install

Linux: install the `.deb`, `.rpm`, `.apk` or `.pkg.tar.zst` from the releases
page, then run `dbox service enable` once your config is ready.

macOS:

```sh
brew install OWNER/tap/dbox
```

From source, with Go 1.26 or newer:

```sh
go install .
```

## Quick start

```sh
dbox config edit       # write a starter config and open it in $EDITOR
dbox check minio       # write, read back and delete a probe object
dbox run --dry-run     # show what the first sync would do
dbox run --once        # first sync, then exit
dbox service enable    # keep it running from login
dbox status
```

## Usage

```
dbox run [--once] [--dry-run]     the daemon; --once reconciles, copies to mirrors and exits
dbox status                       daemon and service state, upload backlog, per-store copies, failed files
dbox retry STORE                  make failed copies on STORE pending again
dbox check STORE                  write, read back and delete a probe object on STORE
dbox promote STORE [--force]      make STORE the primary; the old primary becomes a mirror
dbox service enable|disable       run dbox run at login (launchd agent or systemd user unit)
dbox config [--init]              show the effective config, or write a starter file
dbox config edit                  open the config in $VISUAL or $EDITOR, then check and reload it
dbox reload                       check the config and ask the running daemon to re-read it
dbox version
```

Every command takes `--config FILE`.

## Configuration

The config lives at `~/.config/dbox/config.yml` (`$XDG_CONFIG_HOME` is
respected). `--config` or `DBOX_CONFIG` points at another file.

```yaml
stores:
  minio:
    kind: s3
    role: primary
    bucket: dbox
    region: us-east-1
    endpoint: http://localhost:9000
    path_style: true
    access_key: ${MINIO_ACCESS_KEY}
    secret_key: ${MINIO_SECRET_KEY}

  nas:
    kind: disk
    role: mirror
    root: /Volumes/backup/dbox

sync:
  root: ~/dbox
```

- `kind` is `s3` (AWS, MinIO, R2, B2, …) or `disk`. `role` is `primary` (exactly one), `mirror` or `detached`.
- `${VAR}` is replaced from the environment, which keeps secrets out of the file. Leave the keys out to use the AWS credential chain.
- Unknown keys are an error. `dbox config` prints the effective config with secrets masked.

All keys, their defaults and the `DBOX_SECTION__KEY` overrides are in
[docs/configuration.md](docs/configuration.md).

## Moving to another store

```sh
# 1. add the new store with role: mirror, then
dbox config edit       # validates the config and reloads the daemon
# 2. wait until status shows it at 100%
dbox status
# 3. make it the primary; the old primary becomes a mirror
dbox promote r2
# 4. later, set the old store to role: detached, or remove it
```

`promote` refuses while the new store is missing copies, unless `--force`.
Removing a store never deletes its objects.

## Running in the background

```sh
dbox service enable    # start now and at every login
dbox service disable   # stop and remove it
```

On macOS this installs a launchd agent logging to `~/Library/Logs/dbox.log`;
on Linux a systemd user unit (`journalctl --user -u dbox`). It starts the
`dbox` on your `PATH`, so upgrading the binary is enough.

- **Reload:** `dbox config edit` and `dbox promote` tell the daemon to reload. After editing the file another way, run `dbox reload`.
- **Health:** the daemon serves `/healthz`, `/metrics` and `/status` on `127.0.0.1:7878`. `dbox status` asks `/status` for the upload backlog, so with `daemon.listen` set to an empty string that column shows `-`.
- **Reading the table:** every indexed file is on the primary by definition, so the primary row shows the backlog still to upload under PENDING and failed uploads under FAILED. Mirror rows show copies pending, failed and the share of bytes verified.
- **Failed copies:** `dbox status` lists them. Fix the cause, then run `dbox retry STORE`.

## More

- [docs/configuration.md](docs/configuration.md): every config key, with examples for R2, AWS and a NAS
- [docs/design.md](docs/design.md): architecture, the sync rules, stores and roles, and why dbox doesn't mount S3

## Development

```sh
make minio         # two MinIOs on :9200 and :9300 with a dbox bucket each
make test          # go vet + go test -race ./..., no network
make integration   # S3 store tests against the MinIOs
make run           # daemon on dev.config.yml, syncing ./tmp/box
make snapshot      # archives, Linux packages and the Homebrew cask in dist/
```

## License

MIT, see [LICENSE](LICENSE).
