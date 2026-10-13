# MCAP mapping (v1)

LogGuard reports (`.lgp`) are [MCAP](https://mcap.dev/) files. LogGuard owns
only a channel schema and metadata conventions; the container, framing and
future chunking/indexing come from MCAP.

## File structure emitted in v0.1

```
magic
Header            profile="logguard/1", library="logguard-proto/0.1"
Schema id=1       name="logguard/log-record/v1", encoding="jsonschema"
Channel id=1      schema=1, topic="/logguard/logs", message_encoding="json"
Message *
DataEnd           data_section_crc=0 (not provided)
Footer            summary offsets 0 (no summary section in v0.1)
magic
```

No chunking/compression yet; records are individual MCAP messages. Chunked
zstd output with message indexes is a v0.2 change contained entirely in the
writer — ring and schema stay untouched.

## Record schema

```json
{
  "type": "object",
  "properties": {
    "ticket":   { "type": "integer" },
    "ts_ns":    { "type": "integer" },
    "mono_ns":  { "type": "integer" },
    "tid":      { "type": "integer" },
    "level":    { "type": "string" },
    "category": { "type": "string" },
    "msg":      { "type": "string" }
  }
}
```

`ts_ns` is Unix epoch nanoseconds (display, cross-machine correlation);
`mono_ns` is CLOCK_MONOTONIC nanoseconds (duration analysis within one boot);
`ticket` is the authoritative process-local ordering key.

## Future mapping (planned, not yet written)

| LogGuard concept | MCAP carrier |
|---|---|
| Structured business events | second channel `/logguard/events` with its own schema |
| Snapshot start/end, crash marker | Event-channel marker records (visible on timeline) |
| Session/build/platform metadata | File-level Metadata records |
| request_id / task_id correlation | Fields in the record schema; sidecar index attachment `application/x-logguard-idx` |
| Crash dump reference / embedded dump | Attachment record (opt-in, reference by default) |
| Redaction report | Attachment record `redaction-report.json` |

## Compatibility rules

- Schema name carries `/v1`; incompatible changes take a new schema id and a
  new schema name. Unknown optional fields are ignored by readers.
- Files always carry the trailing magic and standard records, so any
  standards-compliant MCAP reader can scan them even without the LogGuard CLI.
- Verify after touching the writer:

  ```bash
  mcap info crash.lgp
  mcap cat crash.lgp
  ```
