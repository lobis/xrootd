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
- `XrdSecgsiGMAPFunDN`: delete only the locally allocated mapping probe.
- `XrdConfig`: free invalid trailing-argument cipher storage; successful cipher
  storage remains intentionally persistent.
- `XrdConfig::xprot`: release a copied protocol library when `GetRest` rejects
  too many parameters; parser behavior is unchanged except that no allocation
  remains after rejection.
- `XrdAccAccess_Tables`: delete the owned domain-list head `D_List`; `E_List`
  is only a borrowed tail alias and is not deleted separately.  Access-control
  and configuration behavior are unchanged; this only cleans storage when
  tables are destroyed or replaced.
- `XrdSutPFile::RemoveEntries`: release the temporary offset array after the
  removal loop.  Successful and error-visible behavior is unchanged; only
  temporary offset storage is reclaimed.
- `XrdSutPFile::Trim`: retain an internally generated `<name>.bak` with RAII
  through every exit; caller-supplied backup names remain borrowed.  Filesystem
  operations, diagnostics, and return behavior are unchanged.
- `XrdSecProtocolgsi::ErrF`: release the temporary combined debug message after
  synchronous logging consumes it.  Only the debug buffer lifetime changes;
  fallback logging, messages, and error behavior are unchanged.
- `XrdOfsTPC::Init`: pass delegated-auth text directly to `XrdOucEnv::Export`,
  removing only the redundant caller-side copy.  `Export`'s internal `Var=Val`
  allocation remains intentionally persistent for `putenv`; exported value,
  timing, and process-environment behavior are unchanged.
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
  `SetDefaultCiphers` deliberately retains it.  `XrdSecgsiGMAPFunDN` reports
  mappings passed to `gMappings.Add`; that hash owns the installed entries.
- Storage intentionally retained for `putenv`, `XrdOucEnv::Export`, and
  successful `XrdTlsContext::SetDefaultCiphers` calls must not be freed.
