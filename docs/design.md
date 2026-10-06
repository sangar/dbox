# dbox design

A small C daemon that keeps a local folder in sync with one or more stores:
S3-compatible buckets (AWS, MinIO, R2, Backblaze B2) or plain directories.
Sync-folder style: files live on disk, are always available offline, and changes are
pushed in the background. Remote changes are pulled on a poll interval.

Stores follow the immish model: any number of named stores, exactly one is the
**primary**, the rest are **mirrors** or **detached**. Every file is pushed to
the primary and fanned out to every mirror. Migrating backends is "add the new
store as a mirror, wait for backfill, promote it, detach the old one", with no
downtime and no re-upload from scratch.

Targets Linux and macOS from one binary.

## Goals

- Watch one or more local folders and push creates, modifies, renames and
  deletes to S3 shortly after they happen.
- Pull changes made elsewhere (another machine, the S3 console) down to disk.
- Survive restarts: reconcile on startup so nothing is lost while the daemon
  was down.
- Several stores at once with one primary, so a backend can be swapped by
  promoting a mirror. Per-file replica state so backfill and migration are
  resumable and verifiable.
- Single static binary, no root, no kernel extensions.

## Non-goals (for the POC)

- Real conflict resolution. Last writer wins, with the loser kept as a
  `name.conflict-<host>-<timestamp>` sibling.
- Sharing, permissions, selective sync, bandwidth throttling.
- Block-level delta upload. Whole files are uploaded, multipart above a
  threshold.
- Windows.

## Was mounting S3 as a filesystem considered?

Yes. The candidates are FUSE-based mounts:

| Tool | Linux | macOS | Notes |
|---|---|---|---|
| `mountpoint-s3` (AWS) | yes | no | Sequential writes only, no rename, no edit in place. |
| `s3fs-fuse` | yes | needs macFUSE | Full POSIX-ish, slow metadata, weak caching. |
| `goofys` | yes | needs macFUSE | Fast, but relaxed POSIX semantics, unmaintained. |
| `rclone mount` | yes | needs macFUSE | Best of the bunch. `--vfs-cache-mode full` gives a local write-back cache. |

Why a mount does not fit the sync-folder model:

1. **Files are remote, not local.** Offline you have nothing, or only whatever
   the VFS cache happened to keep. Sync-folder semantics are the inverse: disk is
   the source of truth, S3 is the backup/transport.
2. **Every `stat` is a network call** unless aggressively cached, which makes
   Finder, `ls -l`, editors and IDE indexers crawl.
3. **macOS requires macFUSE**, a third-party kernel extension that needs a
   reboot and a Security & Privacy approval. Apple's FSKit replacement is only
   on macOS 15+ and rclone/s3fs do not ship FSKit backends yet. On Linux FUSE
   is fine but still a root-installed dependency.
4. **Editor save patterns break.** Vim, VS Code, Xcode and most IDEs save by
   writing a temp file and renaming over the original. S3 has no rename, so
   FUSE layers either reject it (mountpoint-s3) or emulate it with a copy and
   delete, doubling traffic and leaving windows where the file is missing.
5. **Conflict handling is nil.** Two machines writing the same key via a mount
   silently clobber each other. A sync daemon at least sees both versions.

Where a mount *is* the better answer: a single machine that needs read-mostly
access to a huge bucket that will never fit on local disk (media libraries,
datasets). That is a different product. If that use case shows up later,
shelling out to `rclone mount` is the pragmatic answer, not writing a FUSE filesystem.

Decision: **watcher + local index + uploader**, no mount.

## Stores and roles

Borrowed from immish (`docs/MULTIPLE_STORE.md` there). A store is a named
backend with a `kind` (`s3` or `disk`) and a `role`:

| role | receives new files | pulled from | backfilled |
|---|---|---|---|
| `primary` (exactly one) | yes | yes | n/a |
| `mirror` | yes, fan-out after primary | no | yes, to full copy |
| `detached` | no | no | no |

Rules:

- The store `name` is stable and is what the index records. Renaming a store
  in config is treated as removing one and adding another.
- Only the primary is polled for remote changes. Mirrors are write-only from
  the daemon's point of view; a second machine syncing the same folder talks
  to the same primary.
- A mirror that falls behind never blocks the primary. Each store gets its own
  queue and workers, so a slow B2 mirror cannot delay the MinIO primary.
- A `disk` store in any role. Useful for tests (no MinIO needed), and as a
  mirror onto a NAS mount.
- Promotion is a config edit plus restart, or `SIGHUP`. The daemon refuses to
  promote a store whose replica table shows unverified files and says how
  many. `--force` overrides.
- The old primary becomes a mirror on promotion, same as immish. Change it to
  `detached` once you are happy, then delete it from config when you no
  longer want the daemon to know about it.

### Migration walkthrough

Moving from MinIO at home to Cloudflare R2:

1. Add `r2` to `stores` with `role: mirror`. Restart or `SIGHUP`.
2. The daemon backfills: every file whose replica row for `r2` is missing or
   `pending` is copied from the primary (not from disk, so a file that was
   deleted locally but is still remote also travels). Progress is visible in
   `dbox status` and on `/metrics`.
3. When `dbox status` shows `r2` at 100% verified, set `r2` to `primary` and
   `minio` to `mirror`. Restart or `SIGHUP`.
4. New files now go to R2 first. Polling comes from R2. MinIO still receives
   copies.
5. Set `minio` to `detached`, then remove it. Its objects are left in place;
   dbox never deletes a bucket's contents when a store is removed.

Rollback at any step is the reverse edit.

## Architecture

```
            ┌────────────┐   events    ┌───────────┐  paths   ┌──────────┐
 local fs ─►│  watcher   │────────────►│ debouncer │─────────►│  queue   │
            │ (platform) │             │ per path  │          │ (dedup)  │
            └────────────┘             └───────────┘          └────┬─────┘
                                                                   │
            ┌────────────┐                                    ┌────▼─────┐
            │  poller    │──── primary listing diff ─────────►│ primary  │◄─► primary store
            │ (interval) │                                    │ workers  │
            └────────────┘                                    └────┬─────┘
                                                                   │ replica rows
            ┌────────────┐                                    ┌────▼─────┐
            │ backfiller │──── unverified replicas ──────────►│ mirror   │──► mirror A
            │ (per store)│                                    │ workers  │──► mirror B
            └────────────┘                                    └────┬─────┘
                                                                   │
                                                             ┌─────▼─────┐
                                                             │   index   │
                                                             │ (sqlite)  │
                                                             └───────────┘
```

### Components

**watcher** — `src/watch.c` over one backend per platform in `src/platform/`:
inotify on Linux (`linux.c`) and FSEvents on macOS (`macos.c`). inotify is not
recursive, so the tree is walked at start and every directory added; new
directories are added as they appear in create events. FSEvents watches the
root recursively and costs no descriptor per directory. Ignore patterns
(`.git`, `.DS_Store`, `*.swp`, `~$*`, the index file itself) are applied here
so noise never enters the pipeline.

Platform caveats:

- Linux: `fs.inotify.max_user_watches` defaults to 8192 on some distros. The
  Linux packages ship a sysctl snippet from `packaging/` that raises it.
- The daemon raises the soft `RLIMIT_NOFILE` to the hard limit at startup.

**debouncer** — a per-path deadline on one timer thread. Each event resets it;
when it fires (default 750ms) the path is sent to the queue. Coalesces the
burst of Write events during a save, and the Rename+Create pair from
atomic-save editors. On expiry the path is re-stated so the pipeline acts on
what is actually on disk now, not on what the event said.

**index** — SQLite. Two tables:

```sql
CREATE TABLE files (
  path        TEXT PRIMARY KEY,   -- relative, forward slashes
  size        INTEGER NOT NULL,
  mtime_ns    INTEGER NOT NULL,
  sha256      TEXT    NOT NULL,
  synced_at   INTEGER NOT NULL,
  deleted     INTEGER NOT NULL DEFAULT 0  -- tombstone until every store dropped its copy
);

CREATE TABLE replicas (
  path        TEXT    NOT NULL REFERENCES files(path) ON DELETE CASCADE,
  store       TEXT    NOT NULL,   -- store name from config
  state       TEXT    NOT NULL,   -- pending | verified | failed
  etag        TEXT,               -- as returned by the store, opaque
  verified_at INTEGER,
  last_error  TEXT,
  attempts        INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (path, store)
);
```

Stored at `<root>/.dbox/index.db`. `files` answers "has this file actually
changed since we last hashed it" (compare size+mtime first, hash only if they
differ) and "what did we have last time" (so deletes made while the daemon was
down are detected on reconcile). `replicas` is immish's `blob_replicas`: one
row per file per store, so backfill, promotion checks and `dbox status` are
plain queries.

**primary workers** — N threads reading from the queue. Per path:

1. `stat`. Missing → delete from primary, turn the `files` row into a
   tombstone and every mirror row `pending`, so mirrors delete their copies.
   The tombstone is removed once no replica row is left. A path with no row
   of its own but rows under it was a directory, and each file under it is
   queued.
2. size+mtime match index → skip.
3. Hash. Hash matches index → update mtime in index, skip.
4. Upload to the primary: one `PutObject` below `part_size`, a multipart
   upload above it. Set metadata `x-amz-meta-sha256` and `x-amz-meta-mtime`
   so the remote side carries enough to compare without downloading.
5. Stat again. If size or mtime moved during the upload, record nothing and
   queue the path again, so the index never describes a version other than
   the one the primary holds. Otherwise update `files`, set the primary
   replica row `verified`, and insert a `pending` row for every mirror.

A path already in flight is marked so a second event for it re-queues once
the current operation finishes rather than racing.

A failed upload is recorded in `upload_failures` with its error and retried
with the same backoff as mirror copies: 20s, 40s, 80s, 160s, then 320s. Every
`pull_interval` the daemon queues the failures whose backoff has passed.
After five attempts the path waits for `dbox retry <primary>` or a change to
the file; `dbox status` lists it under the primary. The row is removed when
the file is synced or deleted.

**mirror workers** — one small pool per mirror, fed by that store's `pending`
replica rows. Each job copies the object from the primary store to the mirror
(not from disk, see migration above), `Head`s it on the mirror to confirm size
and the sha256 metadata, then marks the row `verified`. Failures go to
`failed` with `last_error` and retry with backoff, five attempts, then stay
`failed` until `dbox retry <store>` or the next backfill pass.

**backfiller** — runs at startup, on `SIGHUP`, and every `backfill_interval`
for each mirror. It inserts `pending` rows for any file in `files` that has no
replica row for that store, then lets the mirror workers drain them. It is
idempotent and resumable. This is how a freshly added mirror gets a full copy
and how a mirror that was offline for a week catches up.

**poller** — every `pull_interval` lists the **primary** with `ListObjectsV2`
and diffs against the index:

- key not in index → download to a temp file in `.dbox/tmp`, rename into
  place, add to index with the primary replica `verified` and mirrors
  `pending`. The watcher sees the rename; the debouncer re-stats, finds
  size+mtime match the fresh index row and skips. No echo upload.
- key in index with different ETag and local file unchanged since
  `synced_at` → download, same as above.
- key in index with different ETag and local file *also* changed → conflict.
  Local wins; remote copy is downloaded as `name.conflict-<host>-<ts>` and
  local is uploaded.
- key in index but missing remotely → deleted elsewhere. Delete local file,
  remove index row.

S3 has no change feed without SNS/SQS/EventBridge, so polling is the portable
choice. 30s to 60s is fine for a POC. Listing a 100k-key bucket costs about
100 requests, which is cheap.

**reconcile** — runs once at startup before the watcher starts accepting
events. It is the poller diff plus a local walk, in both directions, against
the primary. Then for each mirror it lists the store and corrects the replica
rows: objects present with matching sha256 metadata become `verified` (adopts
copies made by rclone or bucket replication), rows for objects that are gone
go back to `pending`. This is what recovers from a crash, a laptop lid close,
or a first run on a second machine with an empty folder.

**store registry** — builds one client per store from config and hands them
out by name. Config is re-read on `SIGHUP` (sent by `dbox config edit` and `dbox promote`); a changed store is rebuilt, a
removed store's workers are stopped and its replica rows left in place (they
are harmless and come back if the store is re-added under the same name).

**daemon** — a `Ctx` that SIGINT/SIGTERM cancel, structured key=value logs,
a `/healthz`, `/metrics` and `/status` HTTP listener on localhost
(optional). `dbox status` reads the upload backlog from `/status`.
Supervised by launchd on macOS and systemd on Linux; no self-daemonising.

## Sync algorithm summary

| Event | Local state | Remote state | Action |
|---|---|---|---|
| fs event, file exists, hash changed | changed | — | upload |
| fs event, file missing | gone | — | delete remote |
| poll, key new | absent | present | download |
| poll, etag changed | unchanged | changed | download |
| poll, etag changed | changed | changed | conflict copy, upload local |
| poll, key gone, Head confirms | present, synced | absent | delete local |
| reconcile, local newer than index | changed | ? | upload |
| reconcile, in index, missing locally | gone | present | delete remote |

Keys are `prefix + relative path` with `/` separators, where `prefix` is per
store. No encoding of the path beyond that; paths that S3 rejects (very long,
control characters) are logged and skipped.

Mirror actions follow from replica rows rather than events:

| replica state | action |
|---|---|
| `pending`, file exists in `files` | copy primary → mirror, verify, `verified` |
| `pending`, file is a tombstone | delete on mirror, drop row |
| `failed`, under 5 attempts | retry with backoff |
| `failed`, 5 attempts | leave, surface in `dbox status` |

## Project layout

```
dbox/
  src/main.c                command line: subcommands and flag parsing, the reload loop
  src/config.c              config.yml: ${VAR} expansion, DBOX_SECTION__KEY overrides, defaults, validation, promote
  src/yml.c                 libyaml read into a tree that remembers where each scalar sits in the file
  src/ignore.c              ignore patterns from config, always .dbox/
  src/debounce.c            per-path quiet period on one timer thread
  src/index.c               SQLite: files, replicas, upload failures
  src/store.h               the Store interface; disk.c and s3.c implement it
  src/http.c                the HTTP client: the one module that talks to libcurl
  src/s3.c                  S3 over http.c: Signature V4, metadata, multipart above part_size, paginated listing
  src/watch.c               recursive watcher over the platform's file system events
  src/queue.c               the de-duplicating path queue and per-path locks
  src/engine.c              reconcile, the primary workers, poll, conflicts, downloads
  src/mirror.c              mirror workers, backfill adoption, verification
  src/daemon.c              pid file, /healthz /metrics /status listener, status client
  src/service.c             `service enable|disable`: launchd agent / systemd user unit
  src/error.h               the Error enum every fallible function returns
  src/alloc.c               the heap allocator; arena.c for objects that die together
  src/platform/             every OS call: posix.c, plus macos.c (FSEvents) and linux.c (inotify)
  src/util.c, strbuf.c, strmap.c, sha256.c, ctx.c, log.c   building blocks
  deps/sqlite, deps/libyaml vendored sources at the versions in deps.lock
  tests/test_dbox.c         config, index, ignore, debounce, engine scenarios, daemon, service
  tests/test_s3.c           the S3 store against the compose MinIOs
  docker-compose.yml        two MinIO instances for local dev and tests
  Makefile
```

Threads replace goroutines: a fixed set of primary workers reads the queue,
one loop per mirror drains its pending replicas, and short-lived worker sets
run the parallel parts of a reconcile. Cancellation is a `Ctx` passed through
every call; its condition variable is also what the queue and the mirror
loops wait on, so cancelling wakes everything at once.

`src/store.h` exposes a five-method interface (`put`, `get`, `del`, `list`,
`head`) with `s3` and `disk` implementations. Unit tests use `disk`
stores in temp directories; MinIO is only needed for the integration suite. Mirror copy
is `Get` from one store piped into `Put` on another, so adding a new backend
kind (SFTP, WebDAV) is one file and never touches the sync logic.

## Dependencies

| Purpose | Library | From |
|---|---|---|
| fs events | inotify (Linux), CoreServices FSEvents (macOS) | the OS, in `src/platform/` |
| HTTP | libcurl, wrapped by `src/http.c`; `s3.c` adds the Signature V4 signer | the system, for its TLS stack and certificate store |
| index | SQLite, wrapped by `src/index.c` | `deps/sqlite`, pinned in `deps.lock` |
| config | libyaml, wrapped by `src/yml.c` | `deps/libyaml`, pinned in `deps.lock` |

Everything else is C23 and POSIX. Each library is included from exactly one
wrapper module, so swapping one changes one file. Credentials come from the config,
`AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` (with `AWS_SESSION_TOKEN`), or
`~/.aws/credentials`; instance metadata and SSO are not supported. Requests
are retried three times on transport errors and 5xx responses.

## Running as a daemon

Same as eind: the binary installs itself as a per-user login service, no
hand-written unit files.

```sh
dbox service enable           # start now and at every login
dbox service disable          # stop and remove it
dbox status                   # shows whether the daemon answers and whether the service is installed
```

`src/service.c` is a port of eind's `internal/service` package:

- **macOS** writes `~/Library/LaunchAgents/dbox.plist` with `RunAtLoad` and
  `KeepAlive`, logging to `~/Library/Logs/dbox.log`, then
  `launchctl bootstrap gui/<uid>`. Enable first runs `bootout` and waits up
  to 15s for the old registration to disappear, because `launchctl` returns
  before the agent is gone and a bootstrap while it lingers fails with an I/O
  error.
- **Linux** writes `~/.config/systemd/user/dbox.service` (respecting
  `$XDG_CONFIG_HOME`) with `Restart=on-failure`, then
  `systemctl --user daemon-reload` and `enable --now`. Disable is the reverse
  plus a final `daemon-reload`.
- **Executable path.** The unit starts the `dbox` found on `PATH` when that
  is the same file as the running binary, otherwise the running binary's
  absolute path. Upgrading the binary in place then needs no re-enable.
- **Config path** is not baked into the unit. The daemon resolves
  `~/.config/dbox/config.yml` itself, so editing config and `SIGHUP` (or
  `dbox promote`) is enough.
- Other platforms get a clear error telling the user to start `dbox run`
  from their session startup.

What it writes, for reference:

```xml
<!-- ~/Library/LaunchAgents/dbox.plist -->
<plist version="1.0"><dict>
  <key>Label</key><string>dbox</string>
  <key>ProgramArguments</key><array><string>/opt/homebrew/bin/dbox</string><string>run</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/Users/gard/Library/Logs/dbox.log</string>
  <key>StandardErrorPath</key><string>/Users/gard/Library/Logs/dbox.log</string>
</dict></plist>
```

```ini
# ~/.config/systemd/user/dbox.service
[Unit]
Description=dbox folder sync
After=network-online.target

[Service]
ExecStart="/usr/local/bin/dbox" run
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

The Linux packages (`.deb`, `.rpm`, `.apk`, Arch) ship
`/usr/lib/systemd/user/dbox.service` and a sysctl snippet for
`fs.inotify.max_user_watches` from `packaging/`. Unlike eind's, the unit is
not enabled on install, because dbox needs a config first.

## Testing

- **Unit** (`make test`): debouncer coalescing, index tombstones and
  conditional verify, ignore matching, config validation (two primaries,
  no primary, bad names, missing env vars, unknown keys) and `Promote`
  keeping comments. Engine scenarios run against `disk` stores in temp
  directories, with two machines sharing stores where needed: push to
  primary and mirror, local delete reaches every store, edits and deletes
  travel between machines, identical file on a second machine is adopted,
  edit on both sides keeps local and saves a conflict copy, add mirror →
  backfill → promote → detach, mirror adopts copies made by other tools,
  mirror falls back to the local file, and the live daemon handling a new
  directory, an editor-style rename save, a remote change and a removed tree.
- **Integration** (`make minio && make integration`): the S3 store against
  both compose MinIOs, metadata round trip, not-found mapping, and a 12 MiB
  multipart copy streamed from one MinIO's `Get` into the other's `Put`.
- **Platform:** planned CI matrix on `ubuntu-latest` and `macos-latest`. The watcher
  tests are the only ones that differ meaningfully between inotify and FSEvents.
  `src/service.c` is tested with the command runner swapped for a recorder,
  as in eind, so no real `launchctl` or `systemctl` is invoked.

## Open questions

- Should a mirror ever be polled, so that two machines can use different
  primaries against the same set of stores? No for the POC; it turns every
  mirror into a potential source of conflicts.
- Should mirror copies stream through the daemon host, as immish does, or use
  server-side copy when source and target are the same provider? Through the
  host for the POC; same-provider copy is an optimisation for a later pass.

- Should the daemon follow symlinks? Default no; most sync tools do not either.
- Hash choice: SHA-256 is fine on Apple Silicon and modern x86. BLAKE3 is
  faster but adds a dependency. Decide after measuring on a 10 GB tree.
- Should `.dbox/index.db` be excluded from Time Machine? Probably, via
  `tmutil addexclusion`, done at first run.
