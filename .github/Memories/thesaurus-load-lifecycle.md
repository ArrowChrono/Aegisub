# Shared thesaurus load lifecycle

- `Thesaurus::OnLanguageChanged` cancels the previous request before resetting
  the dictionary or taking any early return. Empty language and missing complete
  idx/dat pairs invalidate pending publication just like a valid language switch.
  Moving cancellation after path validation lets an old result restore a
  dictionary under a disabled or missing-language setting.
- The stale-publication window originated in `dbf5af1d4914` when construction
  became asynchronous. `548fbd814` added cancellation after the early returns;
  `61ef27848` preserved that ordering while improving lifetime ownership.
- A worker owns its constructed dictionary and captured cancel token. The wrapper
  owns its current dictionary and UI activation lifetime; workers borrow its weak lifetime. Language cancellation and
  object retirement protect different boundaries; keep both. Do not replace
  shared cancellation with consumer-side menu guards.
- `Lookup` returning empty and `GetLanguageList` membership do not prove readiness.
  No loading-state API is implied by the cancellation repair. `OnPathChanged`
  still only invalidates the language-list cache; there is no path hot reload.
- `tests/tests/thesaurus_loader.cpp` exercises the real wrapper and UTF-8 idx/dat
  files. Its controlled main queue releases a successfully constructed pending
  result only after the language/owner transition. It covers normal grouped
  results, Disable, missing language, reversed A/B publication and owner disposal.
  An empty lookup alone is not a load-completion oracle. Test cleanup retires the
  owner, releases pending synchronous callbacks and joins background workers
  before restoring globals or deleting dictionary files.
