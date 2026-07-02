# `xrd` CLI — project plan

**Status:** living document — update as decisions land.
**Branch / PR:** `lobis:xrd-cli` → [xrootd/xrootd#2827](https://github.com/xrootd/xrootd/pull/2827)
**Related:** [xrootd/xrootd#2836](https://github.com/xrootd/xrootd/pull/2836) (Tape REST client in `XrdClHttp` + Python `TapeClient`), [lobis/gfal](https://github.com/lobis/gfal) (discontinued Python rewrite; reference assets), [gfal2-util](https://github.com/cern-fts/gfal2-util) (upstream reference).

---

## 1. Goal

Ship a single native command-line client, `xrd <command>`, distributed with the
XRootD client packages, that is a **drop-in CLI replacement for gfal2-util**
for the protocols XRootD speaks:

- `root://` / `xroot://` (native)
- `http://` / `https://` / `dav://` / `davs://` (via the `XrdClHttp` client plugin)
- local files (`file://` and bare paths)

"Drop-in" means: a script written against `gfal-ls`, `gfal-copy`, `gfal-stat`,
etc. can switch to `xrd ls`, `xrd copy`, `xrd stat` by changing the executable
name only — same positionals, same flags (accepted everywhere, ignored with a
warning where gfal2/GridFTP-specific), same output shape, same POSIX-errno exit
codes.

### Non-goals

- No library API compatibility with gfal2 — this is CLI-only. Programmatic
  users get the XrdCl C++ API and the Python bindings.
- No SRM, GridFTP, or LFC support. Flags specific to those stacks are accepted
  and ignored (with a warning at `-v`), never errors.
- No hyphenated executables (`xrd-ls`, `gfal-ls` shims). One binary, one
  `xrd <command>` surface. This mirrors the decision already made in the gfal
  rewrite (`gfal <command>` only) and keeps packaging trivial.

---

## 2. Where things stand (branch inventory)

The `xrd-cli` branch currently contains:

| Piece | State |
|---|---|
| `src/XrdApps/XrdCli.cc` — CLI11-based `xrd` binary | scaffold + 3 implemented commands |
| `xrd stat` | implemented (local + remote via `XrdCl::FileSystem::Stat`), tested (`tests/XrdApps/xrd-stat.sh`) |
| `xrd sum` | implemented (remote `Query(Checksum)` + local calculation via `XrdCks`/zlib) |
| `xrd archivepoll` | implemented against `XrdCl::TapeRestClient` (discovery + `archiveinfo`) |
| `xrd copy` | thin delegation to the `xrdcp` engine (`XrdCl::RunXrdCp` extracted into `XrdClCopy.hh`) — **no gfal flag mapping yet** |
| 11 other commands | registered with help text, `exit 2` "not implemented yet" |
| `src/XrdCl/XrdClTapeRest.{cc,hh}` | synchronous WLCG Tape REST client (Discover, ArchiveInfo) in **XrdCl core**, adds `find_package(CURL REQUIRED)` |
| Python `taperest.py` + `PyXRootDTapeRest.cc` | direct bindings to `TapeRestClient` |
| `docs/man/xrd.1` | man page covering scaffold + implemented commands |
| Packaging | RPM spec, debian installs, CI workflow updates |

---

## 3. Prior art and what we reuse

### 3.1 The gfal Python rewrite (`~/git/gfal`, github.com/lobis/gfal)

The rewrite is discontinued as a product, but it embodies months of
reverse-engineering of gfal2-util behavior. It is our **specification and test
oracle**, not a source of runtime code (it is Python/fsspec; `xrd` is C++).
Concretely reusable, in priority order:

1. **`docs/gfal2-util-help-reference.md`** — full `--help` output of every
   gfal2-util command captured on lxplus, plus a per-flag support matrix
   (supported / accepted-but-ignored / intentionally omitted). This is the
   canonical flag-level spec. → **Action:** vendor a copy into this repo
   (e.g. `docs/xrd-cli-gfal2-reference.md`) so the spec travels with the code
   and CI can audit `--help` output against it.
2. **`docs/gfal2-command-compatibility-audit.md`** — per-command compatibility
   verdicts and the list of intentional extensions. Use it as the template for
   an equivalent `xrd` audit as commands land.
3. **Exit-code mapping** (`src/gfal/cli/base.py:exception_exit_code`,
   `tests/test_exit_codes.py`) — gfal2 exits with POSIX errno values, not 0/1.
   Port the mapping to a C++ helper (XrdCl `XRootDStatus` → errno; HTTP status
   → errno). See §4.3.
4. **Test scenarios** — the pytest suite encodes hundreds of behavioral
   details (ENOENT on missing file, `-p` mkdir semantics, `--from-file`
   parsing, broken-pipe handling in `cat`, chained-destination copies, …).
   Port scenario-by-scenario into the shell-test pattern already established
   by `tests/XrdApps/xrd-stat.sh`.
5. **Comparison harness** — the gfal repo has Docker-based side-by-side runs
   against real gfal2 (eospublic/eospilot, EOS macaroon TPC traces in
   `docs/http-tpc-token-trace.md`). Keep this harness alive **in the gfal
   repo**, retargeted at the `xrd` binary, as the out-of-tree acceptance
   suite (it needs CERN infrastructure and can't run in xrootd CI).
6. **Protocol quirk knowledge** — double-slash absolute paths in root URLs,
   EOS returning 403 on HTTP directory listings, macaroon/token issuance
   sequences, checksum-type naming differences. Sprinkle into code comments
   and the audit doc as each command hits the corresponding case.

### 3.2 Existing XRootD tools

`xrd` should be a *front-end*, not a re-implementation:

- `xrd copy` wraps the `xrdcp` engine (already done via `RunXrdCp`). The
  remaining work is a **flag translation layer** (§6, copy row).
- `xrd ls/stat/rm/mkdir/rename/chmod/cat/save/xattr` map ~1:1 onto
  `XrdCl::FileSystem` / `XrdCl::File` calls (the same ones `xrdfs` uses).
  HTTP support comes "for free" through the `XrdClHttp` plugin as long as we
  go through `XrdCl` — this is the core architectural bet and why no curl
  code should live in `XrdCli.cc` itself.

### 3.3 Tape REST: one client, not two ⚠️ *needs decision*

There are currently **two independent WLCG Tape REST implementations** in
flight:

| | PR #2827 (this branch) | PR #2836 (`python-tape-rest-dirac`) |
|---|---|---|
| Location | `XrdCl` core (`XrdClTapeRest.cc`) | `XrdClHttp` plugin (`XrdClHttpTape.cc`) |
| Scope | Discover + ArchiveInfo | full: stage, status, cancel, delete, release, archiveinfo |
| Build impact | `find_package(CURL REQUIRED)` **in XrdCl core** | none (XrdClHttp already links curl) |
| Hardening | basic | redirect credential scoping, WLCG token discovery, header callout, discovery cache, async delivery |
| Python | direct bindings (`taperest.py`) | routed through `FileSystem.prepare/query` (`tape.py`) |

Making curl a hard requirement of `libXrdCl` is a real packaging change that
upstream may reject, and maintaining two clients is a bug farm. **Proposal:**
converge on the #2836 implementation (it is a superset and better hardened),
expose what the CLI needs, and drop `XrdClTapeRest.*` from this branch. Two
mechanical options for how the CLI reaches it:

- (a) `xrd` links/loads `XrdClHttp` helpers directly, or
- (b) `xrd` uses the `FileSystem::Prepare`/`Query(Opaque)` plumbing that
  #2836 already routes tape operations through (same path the Python
  `TapeClient` uses) — no new linkage at all.

**Resolved (2026-07-02): option (b).** The `xrd-cli-v2` branch is built on
top of the #2836 branch; `XrdClTapeRest.*`, the curl dependency on XrdCl core,
and the `taperest` Python bindings are dropped, and `xrd archivepoll` issues
`Query(Opaque, "tape.archiveinfo\n<urls>")` through `XrdCl::FileSystem`,
matching the Python `TapeClient`.

---

## 4. Design principles (decided unless marked open)

### 4.1 Command surface

One binary, `xrd <command>`. The 15 commands mirror gfal2-util's suite:

`ls, copy, rm, cat, stat, rename, mkdir, chmod, sum, xattr, save,
bringonline, archivepoll, evict, token`

Aliases where gfal2 has them: `xrd cp` ≡ `xrd copy`. No other invented
commands until the core 15 are done (the gfal repo's `mount`/TUI extensions
are explicitly out of scope here).

### 4.2 Common flags — every subcommand accepts all of these

| Flag | Behavior in `xrd` |
|---|---|
| `-h, --help` | CLI11 help |
| `-V, --version` | version and exit |
| `-v, --verbose` | repeatable; maps to XrdCl log levels (`-v`→Info, `-vv`→Debug, `-vvv`→Dump) |
| `-t, --timeout` | operation timeout, default 1800 s (gfal2 default) |
| `-E, --cert` / `--key` | X.509 credential; also honor `X509_USER_PROXY`/`/tmp/x509up_u<uid>` auto-detection like gfal2 |
| `--log-file` | redirect client log |
| `-D, --definition` | accepted, ignored with warning (gfal2 parameter injection) |
| `-C, --client-info` | accepted; map to XrdCl app-info string where possible, else ignore |
| `-4` / `-6` | map to `XRD_NETWORKSTACK` (IPv4/IPv6); best-effort for HTTP |

Already wired for `stat`/`sum`/`archivepoll` — factor into a shared
`AddCommonOptions(CLI::App*)` helper before adding more commands (currently
duplicated per subcommand in `XrdCli.cc`).

### 4.3 Exit codes

gfal2-util exits with POSIX errno values. `xrd` must too — this is the part
scripts depend on most. Port the mapping from the gfal repo:

- `ENOENT(2)` missing file/dir, `EACCES(13)` permission denied,
  `EEXIST(17)` exists, `EISDIR(21)`, `EINVAL(22)` bad arguments,
  `ETIMEDOUT(110)` timeouts, `ECOMM/EIO` transport errors.
- Central helper: `int ExitCodeFor(const XrdCl::XRootDStatus&)` using
  `status.errNo` when set, else mapping `status.code`; HTTP status → errno
  table for the HTTP paths (404→ENOENT, 403→EACCES, 409→EEXIST, …).
- `2` is currently used for "not implemented" — that collides with ENOENT;
  switch stubs to `ENOSYS(38)` or gfal2's actual behavior (verify on lxplus).

### 4.4 Output compatibility

Where gfal2 output is script-parsed (`ls -l`, `stat`, `sum`, `xattr`), match
it byte-for-byte in the default mode; richer output only behind flags. The
gfal repo's `GFAL_CLI_GFAL2=1` compatibility-mode learnings enumerate exactly
which fields scripts read. `xrd stat` already follows gfal2's layout — keep
that bar.

### 4.5 Language & dependencies

C++17, CLI11 (already vendored via build dep), `XrdCl` + `XrdClHttp` only.
No curl in `XrdCl` core (§3.3), no new hard deps in the client package beyond
what the branch already negotiated.

---

## 5. Command matrix

Status legend: ✅ done · 🚧 partial · 📋 planned · ❓ open question

| Command | Status | Backing API | Notes / gotchas from gfal work |
|---|---|---|---|
| `stat` | ✅ | `FileSystem::Stat`, local `stat(2)` | keep gfal2 field layout; mode synthesis for backends without one |
| `sum` | ✅ | `Query(Checksum)`, local XrdCks/zlib | checksum-type name normalization (ADLER32 vs adler32); remote-first, local fallback |
| `archivepoll` | ✅ | `Query(Opaque, tape.archiveinfo)` via XrdClHttp (#2836) | re-plumbed onto the #2836 plumbing; `--polling-timeout` loop implemented |
| `copy` (`cp`) | 🚧 | `RunXrdCp` | **largest work item.** Needs gfal-copy→xrdcp flag translation: `-f`→`--force`, `-p`→`--parents`, `-K alg[:val]`→`--cksum`, `--checksum-mode`, `-r`→`--recursive`, `--from-file`, `--dry-run`, chained destinations (src→dst1→dst2, gfal-only: loop), `--copy-mode pull/push/streamed`→`--tpc [only] delegate`-family, `--just-copy`, `--abort-on-failure` (multi-source), accept-and-warn: `-n/--nbstreams`(→`--streams`? verify semantics), `--tcp-buffersize`, spacetokens, `--scitag`, `--evict` (xrdcp supports evict on xroot). Progress-bar parity is *not* required (gfal2 shows one; xrdcp's differs — treat as cosmetic). |
| `ls` | ✅ | `FileSystem::DirList` (+`Stat` for files) | gfal `-l` long format implemented (incl. `--full-time`→long-iso quirk); `--xattr`/`--color` accepted, not implemented yet |
| `cat` | ✅ | `File::Open/Read` | multiple files, SIGPIPE ignored (EPIPE exit like gfal); `-b/--bytes` accepted no-op |
| `save` | 📋 | `File::Open(Write)/Write` | stdin→file; overwrite semantics per gfal2 |
| `rm` | 📋 | `FileSystem::Rm/RmDir` | `-r` recursive (client-side walk), `--from-file`, `--just-delete`, `--bulk` (sequential is fine — gfal repo shipped that), `--dry-run` |
| `mkdir` | 📋 | `FileSystem::MkDir` | `-p` (MkDirFlags::MakePath), `-m MODE`; EEXIST behavior differences per backend documented in gfal repo |
| `rename` | 📋 | `FileSystem::Mv` | backend-native move only; never copy+delete fallback |
| `chmod` | 📋 | `FileSystem::ChMod` | HTTP: no-op with warning (no POSIX mode) — matches gfal repo decision |
| `xattr` | 🚧 | `FileSystem::ListXAttr/GetXAttr` (+local xattr syscalls) | get/list done (`name = value` + blank line format); set (`attr=value`) deferred to Phase 2; HTTP backend support is limited — clear error |
| `bringonline` | 📋 | Tape REST stage (#2836 client) | `--staging-metadata`, `--desired-request-time`, `--poll`; async token printed like gfal2 |
| `evict` | 📋 | Tape REST release | positional order `file [token]` (gfal repo fixed this order — follow it) |
| `token` | 📋/❓ | HTTPS macaroon request | gfal repo has the full request-construction recipe (`docs/http-tpc-token-trace.md`); needs curl → must live behind `XrdClHttp` or use its helpers, not in the CLI |

---

## 6. Milestones

Each phase = one or more reviewable commits on this branch (or follow-up PRs
if upstream prefers slicing); a phase is "done" when its commands pass their
shell tests + the flag audit.

- **Phase 0 — foundation (done except audit tooling).** CLI11 scaffold,
  packaging, man page, CI, `AddCommonOptions`/`BeginCommand` factoring.
  *Remaining:* central exit-code helper, vendor the gfal2 help reference +
  flag-audit CI job.
- **Phase 1 — read-only commands (done).** `ls`, `cat`, `xattr` (get/list)
  implemented with local shell tests; server-backed integration tests against
  xroot and HTTP backends still pending (ring 2).
- **Phase 2 — mutating commands.** `mkdir`, `rm`, `rename`, `chmod`, `save`,
  `xattr` (set).
- **Phase 3 — copy compatibility.** The flag-translation layer over
  `RunXrdCp`, chained destinations, `--from-file`, recursive copy, checksum
  modes, dry-run. Acceptance: the gfal repo's copy test scenarios ported and
  green for local↔xroot↔http.
- **Phase 4 — tape.** Resolve §3.3 first. Then `bringonline`, `evict`, and
  re-plumb `archivepoll`. Loopback-HTTP-server tests (pattern exists in
  `tests/XrdClHttp/TapeTest.cc` on the #2836 branch).
- **Phase 5 — token.** HTTPS macaroon retrieval per the traced gfal2
  sequence. Decide hosting for the HTTP code (XrdClHttp helper).
- **Phase 6 — compatibility audit & polish.** Regenerate the audit doc,
  side-by-side runs via the gfal-repo harness against lxplus/EOS, man page
  completion, remove `NotImplemented` stubs entirely.

Sequencing rationale: phases 1–2 are low-risk `XrdCl` mappings that build
reviewer confidence; the copy layer (3) is the highest-value/highest-risk item
and benefits from the test infrastructure matured in 1–2; tape/token (4–5)
depend on the cross-PR decision.

---

## 7. Testing strategy

Three rings, cheapest first:

1. **Per-command shell tests** (`tests/XrdApps/xrd-<cmd>.sh`, pattern set by
   `xrd-stat.sh`): local-filesystem behavior, `--help` flag presence, exit
   codes, output format. Run in regular CI, no server needed.
   - Add a **flag-audit generator**: a script that diffs each command's
     `--help` against the vendored gfal2 reference table so a missing flag
     fails CI rather than a human audit.
2. **Server-backed integration tests**: reuse the existing test-cluster
   fixtures under `tests/` (the `XRootD::*`/`XrdClHttp` CTest fixtures) to run
   the same command scenarios against a live xrootd + XrdHttp instance,
   including the HTTP protocol path. Tape/token against the loopback HTTP
   server pattern from `TapeTest.cc`.
3. **Out-of-tree acceptance** (gfal repo, manual/nightly): Docker side-by-side
   gfal2 vs `xrd` comparisons, live EOS endpoints (eospublic/eospilot), TPC
   token traces. This is where "drop-in" is actually proven; results feed the
   audit doc.

Port order for gfal-repo pytest scenarios: exit codes → ls/stat formats →
copy semantics → tape stubs.

---

## 8. Packaging, docs, CI (steady state)

- Binary + man page ship in the client packages (RPM `xrootd-client`, debian
  `xrootd-clients`) — already wired; keep spec/debian/install lists in sync as
  files are added.
- `docs/man/xrd.1` grows a section per command as it lands (same commit).
- CI: shell tests in the existing workflows (already wired for stat); add the
  flag-audit job in Phase 0 cleanup.
- Python bindings: **no `xrd`-specific Python surface.** The `taperest.py`
  bindings on this branch should follow the §3.3 decision (likely superseded
  by #2836's `tape.py`).

---

## 9. Open questions (tracked, with current lean)

1. ~~**Tape REST unification** (§3.3)~~ — **resolved**: #2836 adopted,
   `XrdClTapeRest.*` and the `CURL REQUIRED` on XrdCl core dropped.
2. ~~**`xrd` name collision**~~ — **resolved**: the binary name is `xrd`.
3. **Where does `token` HTTP code live** — XrdClHttp helper vs CLI-private.
   Lean: XrdClHttp helper (curl already there, reusable by others).
4. **`-n/--nbstreams` mapping** — xrdcp `--streams` changes semantics
   (multiple TCP streams) vs gfal2's GridFTP meaning. Verify whether mapping
   or accept-and-ignore is safer for scripts.
5. **Recursive operations on HTTP** — DirList over WebDAV via XrdClHttp is
   in flux upstream; `ls -r`/`rm -r`/`copy -r` on HTTP may need to trail the
   plugin's capabilities. Document per-protocol support in the man page.
6. **Stub exit code** — pick `ENOSYS` vs matching observed gfal2 behavior for
   unsupported operations (needs a quick lxplus check).

---

## 10. How to evolve this document

- Update the command matrix status column in the same commit that changes a
  command.
- Move resolved open questions into §4 (decisions) with a one-line rationale.
- When a phase completes, replace its bullet with a link to the commits/tests
  that closed it.
