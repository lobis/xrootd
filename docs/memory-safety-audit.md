# Memory safety audit (first pass)

This is a conservative, ownership-focused pass.  It intentionally avoids
changing successful-path behavior, callback ownership, or public interfaces.

## Fixed findings

- `XrdPssConfig`: release the local origin string on the empty-host parse exit.
- `XrdSutPFEntry`: make copy assignment safe for self-assignment and copy the
  source name rather than freeing the destination name before rereading it.
- `XrdSutCacheEntry`: make copy assignment safe for self-assignment, copy the
  source name, and preserve all four source buffers by passing their lengths.
  This intentionally corrects assignment, which previously cleared the buffers.
- `XrdNetRegistry`: validate arguments and aliases before duplicating host
  lists, and handle a failed duplication.
- `XrdSecServer`: release the temporary host string on `protbind` parse errors.
- `XrdFrmTransfer`: delete the temporary transfer object after `Start` returns,
  including the no-argument thread path.
- `XrdSutPFile`: release serialization buffers after writes and on the
  post-allocation header error exits.
- `XrdTlsContext`: delete an invalid non-null CRL-refresh clone before retrying.
- `XrdFfsQueue`: recheck task completion in a mutex-held predicate loop so
  spurious condition-variable wakes cannot expose a task to premature free;
  worker ID/thread allocations are also checked and cleaned on failure.
- `XrdOfsPrepGPI`: defer request allocation until after the maximum-file check.
- `XrdClCopy`: delete the per-job result object when `getcwd` fails.
- `XrdHttpProtocol`: clean preload metadata and data buffers on allocation,
  read, and truncation failures.
- `XrdHttpProtocol::GetVOMSData`: restore the captured original name whenever
  it exists after the extractor callback, preventing a leak if the static
  mapping pointer changes during that callback.  Stable configured behavior is
  unchanged.
- `XrdFrmConfig`: clean monitor destinations on parser errors and when
  monitoring is disabled; destinations remain owned by `Defaults` on success.
- `XrdFrmAdminQuery`: avoid allocating the default virtual-place name before
  replacing it with borrowed `getarg()` storage.
- `XrdSsiLogging`: release configured plugin path copies after the plugin has
  consumed them, including error exits.
- `XrdFrcReqFile`: clean locally recovered request chains on a read abort.
- `XrdXrootdConfig`: retain checksum algorithm chains locally until validation
  succeeds, then transfer them to `JobCKTLST`.
- `XrdXrootdConfigMon` / `XrdXrootdGSReal`: reject a failed g-stream network
  destination before registering its monitor identity or scheduling autoflush;
  destruction of that unscheduled partial object releases its relay, buffer,
  and separately allocated text headers.  `Hello` now supports deferred
  registration so invalid streams never enter its static identity list.
  `XrdNetMsg` initializes its default-destination pointer before validation so
  destroying the failed relay is valid.
- `XrdFfsFsinfo`: return `-ENOMEM` from allocation failures, as required by
  the FUSE callback convention, and free transient cache entries that cannot
  be inserted.
- `XrdXmlRdrXml2`: release `name` on every scan-loop iteration, including
  unmatched end elements and named non-element nodes.
- `XrdSecProtocolpwd` / `XrdSecpwdSrvAdmin`: clean temporary salt, serialized,
  debug, and public-key-output buffers without freeing transferred buffers.
  `Serialized(..., 'f')` and `XrdSecCredentials` payloads use `malloc`/`free`,
  while `DoubleHash` releases its copied `new[]` result after `SetBuf`.
  Stored client credentials now follow the protocol object's `Delete()` path.
- `XrdSendQ`: free a newly allocated message when `QMsg` rejects ownership.
- `XrdPfcDirStateSnapshot`: remove calls through a data-file pointer after it
  has already been closed and deleted.
- `XrdSecgsiGMAPFunDN`: delete the locally allocated mapping probe after
  scanning, and delete a newly allocated configuration entry rejected by a
  duplicate hash insertion; accepted entries remain owned by `gMappings`.
- `XrdConfig`: free invalid trailing-argument cipher storage; successful cipher
  storage remains intentionally persistent.
- `XrdConfig::Configure`: release the local default protocol-name copy after
  synchronous `Setup`; `Firstcp` owns its separate duplicate, while intentional
  `tlsciphers` retention is unchanged.
- `XrdConfig::xprot`: release a copied protocol library when `GetRest` rejects
  too many parameters; parser behavior is unchanged except that no allocation
  remains after rejection.
- `XrdAccAccess_Tables`: delete the owned domain-list head `D_List`; `E_List`
  is only a borrowed tail alias and is not deleted separately.  Access-control
  and configuration behavior are unchanged; this only cleans storage when
  tables are destroyed or replaced.
- `XrdAccGroups`: delete a newly built group list when a concurrent duplicate
  cache insertion is rejected; first-entry-wins semantics, returned copies,
  empty results, and list contents remain unchanged.
- `XrdAccGroupList`: clamp constructor counts before copying and zero-filling.
  Oversized counts are safely truncated and negative counts produce an empty
  list instead of undefined out-of-bounds access; valid inputs are unchanged.
- `XrdOucGMap`: delete a newly constructed grid-map entry rejected by a
  duplicate hash insertion; first-record-wins matching, unique-entry ownership,
  parsing, logging, and return behavior are unchanged.  This is separate from
  the `XrdSecgsiGMAPFunDN` mapping-probe cleanup.
- `XrdOucGMap`: release the constructor-owned `XrdOucTrace` during normal and
  invalid-construction destruction.  The error destination (`eDest`/`elogger`)
  remains borrowed; mapping behavior is unchanged and only tracer lifetime is
  corrected.
- `XrdOucGMap::load`: stop identity scans at the record terminator and reject
  empty identities before marker handling.  Malformed records with no unquoted
  separator, an unterminated quote, or an empty identity now log the existing
  incomplete-line error and are skipped instead of reading beyond the record;
  valid quoted and unquoted mappings, including marker-only patterns, retain
  their prior parsing and lookup behavior.
- `XrdSutPFCache::Rehash`: delete a newly allocated index rejected because its
  name is already present in the hash (for example during a concurrent or
  repeated same-second rebuild); accepted keys remain hash-owned.  Lookup,
  first-entry-wins behavior, counters, locking, tracing, timing, and return
  behavior are unchanged; only the unreachable rejected allocation is
  reclaimed.
- `XrdSutPFile::UpdateHashTable`: delete a newly allocated offset rejected when
  an inactive and active record share a name after remove/re-add history;
  first-entry-wins ordering, counters, locking, logging, errors, and returns
  are unchanged, and accepted keys remain hash-owned.  Only unreachable
  rejected storage is reclaimed.
- `XrdSutPFile::RemoveEntries`: release the temporary offset array after the
  removal loop.  Successful and error-visible behavior is unchanged; only
  temporary offset storage is reclaimed.
- `XrdSutPFile::Trim`: retain an internally generated `<name>.bak` with RAII
  through every exit; caller-supplied backup names remain borrowed.  Filesystem
  operations, diagnostics, and return behavior are unchanged.
- `XrdSecProtocolgsi::ErrF`: release the temporary combined debug message after
  synchronous logging consumes it.  Only the debug buffer lifetime changes;
  fallback logging, messages, and error behavior are unchanged.
- `XrdSecProtocolgsi::GetCA`: release a locally allocated `X509Chain` when the
  parser hook is unavailable; borrowed handshake chains remain untouched.  If
  allocation returned null, the guarded `SafeDelete` is harmless.  A missing
  parser hook still returns exactly as before; only unreachable local storage
  is reclaimed.
- `XrdOfsTPC::Init`: pass delegated-auth text directly to `XrdOucEnv::Export`,
  removing only the redundant caller-side copy.  `Export`'s internal `Var=Val`
  allocation remains intentionally persistent for `putenv`; exported value,
  timing, and process-environment behavior are unchanged.
- `XrdCl::XRootDTransport::GenerateEndSession`: transfer the combined signature
  buffer into the request and delete the now-empty signature wrapper.  Only
  wrapper/buffer ownership changes; wire bytes, marshalling, and session
  behavior are unchanged.
- `XrdCl::AioCtx`: retain the copied host list with RAII, release it exactly
  once to completion handling, and destroy the unsubmitted context when an
  `aio_*` call rejects it, using the captured errno for diagnostics and status.
  Synchronous cleanup, asynchronous `LocalFileTask` ownership, callback timing,
  response handling, and successful submission behavior remain unchanged.
- `XrdCl::XRootDSource::FillQueue`: release a chunk buffer when `Read` or
  `PgRead` rejects submission before transferring it to a completion response.
  Submitted buffers retain their existing response/cleanup ownership; queueing,
  status, offsets, and copy behavior are unchanged.
- `XrdCl::FileStateHandler::XAttrOperationImpl`: delete the locally created
  request message when `CreateXAttrBody` rejects the attributes before any
  handler or queue can own it.  The exact validation status and all valid/send
  behavior remain unchanged.
- `XrdCl::FileStateHandler::OpenImpl`: delete the request message when
  `FillFhTempl` fails before any handler or send path can own it.  The same
  failure status/state transition and all valid/send behavior remain unchanged.
- `XrdCl::FileStateHandler::ReOpenFileAtServer`: delete the recovery request
  message when `FillFhTempl` fails before handler or send ownership begins.
  Recovery status/state transitions and successful reopen behavior remain
  unchanged.
- `XrdXrootdGSReal`: remove an unmatched CGI-header formatting conversion.
  Malformed/undefined prior output behavior could append arbitrary memory or
  crash during CGI g-stream construction; it now emits the intended header
  ending in a newline.  Valid intended functionality otherwise remains
  unchanged.
- `XrdCl::MessageUtils::RewriteCGIAndPath`: reject malformed `kXR_mv` payloads
  before size underflow or out-of-bounds access.  Such move requests now remain
  unchanged no-ops, while valid requests retain their existing behavior.
- `XrdOucString`: format into an independently owned temporary and adopt it
  only after success; allocation or formatting failure now returns `-1` with
  the destination unchanged.  `setbuffer` retains the adopted buffer if its
  shrink realloc fails instead of losing ownership.  Valid formatted output is
  unchanged.
- `XrdOucString`: failed growth or shrink reallocations preserve the prior
  value and capacity and abort the requested mutation or resize.  Successful
  resize, assignment, append, and replacement behavior remains unchanged.

## Deferred and expected analyzer reports

- `XrdCl` `HandleResponse`/`QueueTask`, `XrdClHttp`, and `XrdClS3` response
  paths transfer response/status ownership to their handlers.
- `XrdXrootdAioPgrw` wrapper lifetime, `XrdPfc` direct-response-handler
  self-deletion (including synchronous `ReadV` behavior), and `XrdRmcData`
  failed-`Detach` ownership need dedicated semantic tests before changes.
- `XrdOucString`: the remaining pass-by-value temporary destructor report is
  analyzer alias modeling and remains deferred.
- `XrdFfsQueue` normal worker-removal warnings remain deferred: they depend on
  asynchronous dequeue and completion signaling that the analyzer cannot model
  when a task is removed before its completed task is freed.
- `XrdXrootdConfigMon` successful g-stream placement depends on the configured
  environment route; only the invalid-construction path is changed here.
- `XrdConfig` reports successful `tlsciphers` storage as leaked, but
  `SetDefaultCiphers` deliberately retains it.  `XrdSecgsiGMAPFunDN` may still
  report accepted entries passed to `gMappings.Add`; that hash owns them, while
  duplicate rejected entries are explicitly deleted.
- `XrdAcc` capability-list and `SYList` ownership reports remain analyzer false
  positives: capability chains are transferred to owning hashes/tables, and
  `SYList` entries are owned by `S_Hash` and released during table teardown.
- Storage intentionally retained for `putenv`, `XrdOucEnv::Export`, and
  successful `XrdTlsContext::SetDefaultCiphers` calls must not be freed.
