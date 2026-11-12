# nexus-api-tool
A tool for the modding community to work with Nexus Mods pages through the Nexus Mods API.

A small Windows command-line program that reads a Nexus Mods page and prints it as JSON (UTF-8). Made to be easy for
scripts and AI agents to call.

Most commands only read. These ones write something:

- `dl-file` and `save-desc` save a file on your PC, where `--dest` says.
- Every request is logged in `nexus-api-tool-usage.jsonl` beside the exe (see [Rate limits](#rate-limits)).
- `set-desc` replaces a mod page's description, after saving a backup of the current one on your PC.
- `upload-file` adds a file to a mod page.

`set-desc` and `upload-file` change the mod page straight away, so try them with `--dry-run` first.

## Setup

Set one environment variable to your personal API key, from nexusmods.com > Site preferences > API Keys:

```
setx NEXUS_API_KEY "your key"
```

The name is fixed, so the same setup works for everybody. The key is sent only to the Nexus API and is never printed.

## Commands

```
nexus-api-tool <command> <mod id> [arguments] [--game <domain|id>]
nexus-api-tool <command> --list <file> [options]
nexus-api-tool --usage [--verbose] [--per s|m|h|d]
```

The mod id is the number in the mod page's URL. The game is a Nexus domain (`skyrim`, `skyrimspecialedition`) or a game
number. Without `--game` it comes from the `NEXUS_GAME_ID` environment variable, and without that it is
`skyrimspecialedition`.

| Command | Returns | API (key needed for v1 and v3) |
|---|---|---|
| `desc <mod>` | name, author, uploader, version, status, summary, description | v1 |
| `changelog <mod>` | the changelog, per version | v1 |
| `files <mod>` | every non-archived file, grouped into `main`, `update`, `optional`, `miscellaneous`, `old` | GraphQL |
| `archived-files <mod>` | the same for `archived` and `removed` files | GraphQL |
| `file-note <mod> <file#>` | the note on one file | GraphQL |
| `contents <mod> <file#>` | the files inside one archive, with its plugins, BSAs and `single_plugin` | GraphQL; v1 + content preview for files GraphQL hasn't indexed yet |
| `dl-file <mod> <file#> --dest <path>` | downloads the file (needs a Premium account) | v1 + the download |
| `permissions <mod or address>` | the page's Permissions and credits section; see [Permissions](#permissions) | the mod page itself (website, not an API; this copy only) |
| `reqs <mod>` | the mods, external links and DLC this mod requires, and `has_requirements` | GraphQL |
| `dependents <mod>` | the mods that require this mod | GraphQL |
| `list-mods <user>` | the mods a user uploaded for the game, newest first | GraphQL |
| `save-desc <mod> --dest <path>` | saves the description page as a BBCode text file | v3 |
| `set-desc <mod> --file <path>` | **replaces** the description page with a BBCode text file | v3 |
| `upload-file <mod> --file <path> --name <label> --file-version <v>` | uploads an archive | v3 + an upload Cloudflare limits |

`--help --verbose` lists how many calls each command makes, and how many runs a minute, an hour and a day the pacing and
the API limits allow.

Each file has `file_id`, `label`, `version`, `category`, `uploaded`, `uploaded_timestamp`, `note`, `file_name`,
`size_bytes`, `size_kb` and `primary`. Within a category, the newest comes first. `file_name` is null for newer uploads,
where Nexus doesn't report it in the file list. These three commands also work on unpublished mods; `desc` and `changelog`
don't, because only the older API offers them and it refuses those mods.

`--dest` is a folder (it must exist, or end in `\`) or a file name. An existing file is only replaced with `--force`;
`--skip-existing` leaves it and reports the item as skipped. `--max-size <size>` (bytes, or `500KB`, `200MB`, `2GB`) skips
bigger files before anything is downloaded. The download is written to `<name>.part` and renamed once its size has been
checked. The output always gives `file_name` (Nexus's name for the file, also when the file list shows none) and
`saved_as`.

### Many items at once: `--list`

```
nexus-api-tool dl-file --list files.txt --dest D:\Downloads\ --skip-existing --max-size 200MB
nexus-api-tool reqs --list mods.txt
```

Each line of the file holds what the command takes: a mod id, a mod id and a file number, or a user name, separated by
spaces or commas. `#` starts a comment. It works with the read and download commands: `desc`, `changelog`, `files`,
`archived-files`, `file-note`, `contents`, `dl-file`, `reqs`, `dependents`, `list-mods` and `save-desc`. The output is one
JSON object per line: `line`, `args`, `ok`, and `result` or `error`. A failed item doesn't stop the others; a rate limit
does, so the run can be resumed from that line.

### Limits

`reqs`, `dependents` and `list-mods` can be very long: USSEP has over 2,500 dependents. So one call returns 50 entries
by default and at most 200 (`--limit`), which is at most 4 API requests. The output has `total`, `offset`, `returned` and
`next_offset`; pass `--offset <next_offset>` to continue, and stop when it is `null`.

The other read commands are a single request, except `dl-file`, which makes two plus the download.

## Rate limits

The Nexus Mods API allows 2,000 requests an hour and 20,000 a day per API key; v1 and v3 are counted separately. That
is 0.56 a second, 33.3 a minute, 2,000 an hour, or 48,000 a day at the hourly rate, so the daily limit comes first.
GraphQL has no published limit and reports none.

The tool spaces its requests out by itself: queries 1 per 3 seconds, downloads 1 per 4 seconds, uploads 1 per 31
seconds, also across separate runs. `--interval <seconds>` replaces those gaps for one run (`0` = no pause).

Cloudflare's upload limit is inconsistent with their stated instructions e.g. "retry_after: 30". Once this limit is
tripped Cloudflare may impose a limit of 4/hour.

When a request is refused for a rate limit, the tool stops with exit code 3, says whether Nexus Mods or Cloudflare
refused it, and gives `retry_after` in seconds.

`--usage` (also `--rate`, `--rates`, `--limits`, or `--limit` with no command) reports the requests of the last hour, by
API, and what Nexus Mods last said is left of the limits. Uploads have their own section: upload limits are handled by
Cloudflare. `--verbose` adds a breakdown by command; `--per s|m|h|d` (or `sec`, `second`, `min`, `minute`, `hr`, `hour`,
`day`) sets the window.

The data comes from two files beside the exe, or in `%LOCALAPPDATA%
exus-api-tool` when that folder can't be written:
`nexus-api-tool-usage.jsonl` (every request: time, command, API, status, and the limits Nexus reported) and
`nexus-api-tool-state.json` (the time of the last request of each kind, and the last reported limits). The log keeps
8 days once it passes 8 MB.

### Changing a description page

```
nexus-api-tool save-desc 12345 --dest page.bbcode
nexus-api-tool set-desc 12345 --file page.bbcode --dry-run
nexus-api-tool set-desc 12345 --file page.bbcode
```

The text file is plain BBCode in UTF-8, with ordinary line breaks. `set-desc`:

1. Checks the file. HTML tags and entities such as `<br />` or `&amp;` are refused, because Nexus escapes what it is sent and
   they would show on the page as literal text. `--allow-html` overrides this when they really are meant to be shown.
2. Backs up the current page. If the backup can't be written, nothing is changed. `<id>-<time>-description.bbcode` can be
   given back to `set-desc` to restore the page; `.raw.txt` is the text exactly as Nexus stored it. They go in
   `nexus-api-tool-backups` next to your file, or in `--backup-dir`.
3. Replaces the page and reads it back to confirm it matches.

`--dry-run` does the check and the backup and stops. The page's summary isn't touched.

### Permissions

```
nexus-api-tool permissions 12345
nexus-api-tool permissions "https://www.nexusmods.com/skyrimspecialedition/mods/12345"
nexus-api-tool permissions 12345 --html "saved page.mhtml"
```

No Nexus API returns the "Permissions and credits" section, so this command reads it from the mod page. `sections` holds
each heading of that section: the checklist as a list of `state` (`yes`, `no` or `maybe`), `title` and `text`, and the
rest (author notes, file credits, Donation Points) as text. `source` says where it came from.

The tool isn't signed in, so unpublished and hidden pages can't be read from their address. Save one from your browser
(`.html` or `.mhtml`) and pass it with `--html`. If Nexus ever turns the program away from a public page, `source` is
`api_only`, `sections` is null, and `mentions` lists the lines of the description and of the uploader's profile that talk
about permissions, modifying or patches. Mentions are a hint to read further, not a grant.


### Uploading files

```
nexus-api-tool upload-file 12345 --file "My Mod 1.2.7z" --name "My Mod" --file-version 1.2 --dry-run
nexus-api-tool upload-file 12345 --file "My Mod 1.3.7z" --name "My Mod" --file-version 1.3 --replaces 67890 --changelog "Fixed a thing"
```

Without `--replaces` the archive becomes a new file. With it, the archive becomes a new version of that file and the old
one is archived. `--replaces` takes the file number from the `files` command. Other options: `--category` (`main`,
`optional` or `miscellaneous`), `--note`, `--changelog` and `--set-mod-version`.

Names may use letters, digits, spaces and `_ ' ( ) . -`, up to 50 characters. Versions may use letters, digits and `. -`.
Archives must be under 100 MiB. `--dry-run` checks everything and uploads nothing.

Uploads and page changes show on the mod page right away, so try `--dry-run` first.

## Exit codes

`0` ok, `1` the request failed, `2` bad arguments, `3` rate limited (by Nexus Mods or Cloudflare; the error says which and
how long to wait). Errors go to stderr, in plain words; a rate limit also prints `error`, `rate_limited_by` and
`retry_after` as JSON on stdout.

## Building

Needs Visual Studio's C++ tools, CMake, Ninja and vcpkg (`VCPKG_ROOT` set):

```
build.cmd
```

The result is `build\nexus-api-tool.exe`, a single file with no runtime dependencies. On a machine with a firewall
that asks about new programs, it has to be allowed to reach the internet the first time.
