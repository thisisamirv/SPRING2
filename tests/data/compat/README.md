# Backward-compatibility archive fixtures

Frozen archives that the **current** SPRING2 binary must still decompress,
preview, and (when applicable) audit and stream.

## Layout

- `manifest.tsv` — one row per fixture (tab-separated). Integration tests load
  this file and run the listed checks.
- `legacy/` — original Spring (SPRING1) archives with a `.spring` extension.
- `<creator_version>/` — SPRING2 `.sp` goldens minted by that creator version.
  Never regenerate an older directory with a newer binary.

## Minting a new SPRING2 golden set

Before tagging a release (or whenever on-disk archive layout changes in a
backward-compatible way you still want locked):

```bash
python tools/dev/mint_compat_fixtures.py --spring2 path/to/spring2
```

That writes `compat/<VERSION>/` archives with `-t 1` and upserts the matching
manifest rows. Commit the new directory and the updated `manifest.tsv`.

## Rules

1. Do **not** overwrite historical `compat/<old_version>/` trees or rewrite
   `compat/legacy/` with a newer compressor.
2. Adding a fixture means the current decompress path must restore the listed
   reference bytes (newline-normalized for plain text; gzip-decoded when
   `refs_gzipped=1`).
3. Parser-only coverage (format 0/1/2, creator semver gates) stays in
   `tests/unit/archive_metadata_version_unit_tests.cpp`.
4. Legacy / historical archive decompress coverage lives only in
   `tests/integration/compat_archive_integration_tests.cpp` (do not re-add
   one-off fixture tests elsewhere).
