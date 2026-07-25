# n8n flow: notemeet → Obsidian

The device POSTs a JSON payload to a webhook whenever a voice note is
transcribed (during Sync) or a meeting summary is finalised. n8n turns that
into a Markdown file in an Obsidian vault.

## Payload

```json
{
  "type": "note",
  "tag": "Work",
  "text": "Pick up milk",
  "timestamp": "2026-07-25T14:30:00Z"
}
```

`type` is `note`, `meeting`, or `test`. Meetings carry `title`, `summary`, and
`transcript` instead of `tag`/`text`. The `test` type comes from the dashboard's
**Send test payload** button and is written to a `test/` subfolder so it never
pollutes real notes.

## Setup

1. Import `notemeet_n8n_workflow.json` into n8n.
2. Edit the `VAULT` constant at the top of the **Build note** node to your
   vault path. It writes to `<VAULT>/notes/`, `<VAULT>/meetings/`, and
   `<VAULT>/test/` — create those directories, or the write node fails.
3. Activate the workflow.
4. In the device dashboard → Settings, set the Webhook URL to the
   **production** URL (`https://<host>/webhook/notemeet`), save, then click
   **Send test payload**.

The button reports the HTTP status the device actually received, so a green
result means the file was written — not merely that n8n accepted the request.

## Why the flow is shaped this way

Three failure modes made the original version fail silently. Each is now
closed, and re-introducing any of them brings the silence back:

**The webhook responds when the workflow finishes** (`responseMode:
"lastNode"`), not immediately. n8n's default replies `200 {"message":"Workflow
was started"}` before running anything, so the device could never distinguish
"note filed" from "workflow exploded on the first node".

**The write uses the native Read/Write File node**, not `require('fs')` in a
Code node. n8n blocks built-in modules unless `NODE_FUNCTION_ALLOW_BUILTIN=fs`
is set in its environment; without it the node throws *after* the webhook has
already returned success.

**An unrecognised `type` throws** instead of being dropped. The original Switch
node had `fallbackOutput: "none"`, so a payload that didn't match `note` or
`meeting` exactly — including the version-dependent `$json.body.type` vs
`$json.type` difference — vanished without a trace.

## If the device reports success but no note appears

The file was written to the filesystem **n8n is running on**. If your vault
lives on a different machine, that's the whole problem: nothing moves it
across on its own.

```bash
# on the n8n host
ls -la /home/anthony/obsidian/notemeet/notes/
```

Files present here but absent in Obsidian means you need a sync layer between
the two machines (Syncthing, an SMB mount, or a git pull), not a workflow fix.

If n8n runs in Docker, also check that the vault path is a mounted volume —
otherwise the writes land inside the container and disappear when it restarts:

```bash
docker exec <n8n-container> ls /home/anthony/obsidian/notemeet/notes/
```

## Legacy

`imap_note.py` belongs to an earlier version of this flow that filed notes into
Apple Notes over iCloud IMAP. It is not used by the workflow above and is kept
only for reference.
