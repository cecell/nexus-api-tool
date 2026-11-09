# nexus-api-tool
A tool to help our friends help us!

A small Windows command-line program that reads a Nexus Mods page and prints it as JSON (UTF-8). Made to be easy for
scripts and AI agents to call. It only reads; the one thing it writes is the file you ask `dl-file` to download.

## Setup

Set one environment variable to your personal API key, from nexusmods.com > Site preferences > API Keys:

```
setx NEXUS_API_KEY "your key"
```

The name is fixed, so the same setup works for everybody. The key is sent only to the Nexus API and is never printed.

## Commands

```
nexus-api-tool <command> <mod id> [arguments] [--game <domain>]
```

The game defaults to `skyrimspecialedition`. The mod id is the number in the mod page's URL.

| Command | Returns | Needs the key |
|---|---|---|
| `desc <mod>` | name, author, uploader, version, status, summary, description | yes |
| `changelog <mod>` | the changelog, per version | yes |
| `files <mod>` | every non-archived file, grouped into `main`, `update`, `optional`, `miscellaneous`, `old` | yes |
| `archived-files <mod>` | the same for `archived` and `removed` files | yes |
| `file-note <mod> <file#>` | the note on one file | yes |
| `dl-file <mod> <file#> --dest <path>` | downloads the file (needs a Premium account) | yes |
| `permissions <mod>` | see [Permissions](#permissions) | no |
| `reqs <mod>` | the mods, external links and DLC this mod requires | no |
| `dependents <mod>` | the mods that require this mod | no |

Each file has `file_id`, `label`, `version`, `category`, `uploaded`, `uploaded_timestamp`, `note`, `file_name`,
`size_bytes`, `size_kb` and `primary`. Within a category, the newest comes first.

`--dest` is a folder (it must exist, or end in `\`) or a file name. An existing file is only replaced with `--force`.
The download is written to `<name>.part` and renamed once its size has been checked.

### Limits

`reqs` and `dependents` can be very long: USSEP has over 2,500 dependents. So one call returns 50 entries by default
and at most 200 (`--limit`), which is at most 4 API requests. The output has `total`, `offset`, `returned` and
`next_offset`; pass `--offset <next_offset>` to continue, and stop when it is `null`.

Every other command is a single API request, except `dl-file`, which makes two plus the download.

### Permissions

No Nexus API returns the page's "Permissions and credits" checklist, and the mod page itself can't be read by a
program (it sits behind a Cloudflare check). So `permissions` returns `checklist: null`, the `page_url` to read it in
a browser, and `mentions`: the lines of the description and of the uploader's profile that talk about permissions,
modifying or patches. Mentions are a hint to read further, not a grant.

## Exit codes

`0` ok, `1` the request failed, `2` bad arguments. Errors go to stderr.

## Building

Needs Visual Studio's C++ tools, CMake, Ninja and vcpkg (`VCPKG_ROOT` set):

```
build.cmd
```

The result is `build\nexus-api-tool.exe`, a single file with no runtime dependencies. On a machine with a firewall
that asks about new programs, it has to be allowed to reach the internet the first time.
