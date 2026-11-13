// nexus-api-tool: read a Nexus Mods page's information from the command line. See README.md.
//
// Sources: the v1 REST API (needs an API key) for description, changelog and downloads, the public v2 GraphQL API for
// file lists, requirements and dependents, and the v3 REST API for changing a mod page. Everything is printed as JSON (UTF-8) on stdout; errors go to stderr.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

// The one environment variable the tool reads its key from. Everybody sets the same name.
constexpr const wchar_t* kKeyEnvVar = L"NEXUS_API_KEY";
constexpr const char* kKeyEnvVarText = "NEXUS_API_KEY";

constexpr const char* kToolName = "nexus-api-tool";
constexpr const char* kToolVersion = APP_VERSION;  // from CMakeLists.txt
constexpr const char* kDefaultGame = "skyrimspecialedition";
constexpr const char* kV1 = "https://api.nexusmods.com/v1";
constexpr const char* kGraphQl = "https://api.nexusmods.com/v2/graphql";
constexpr const char* kV3 = "https://api.nexusmods.com/v3";
constexpr long long kMaxUploadBytes = 100LL * 1024 * 1024;  // larger files need the API's multipart upload
constexpr int kPageSize = 50;  // 100 goes over the GraphQL API's query complexity limit
// One call never asks for more than this, so a mod with thousands of dependents can't run up the API
// rate limit or flood the caller. The output gives next_offset to continue from.
constexpr int kDefaultLimit = 50;
constexpr int kMaxLimit = 200;

constexpr const char* kUsage = R"(nexus-api-tool - work with Nexus Mods pages through the Nexus Mods API; prints JSON

USAGE
  nexus-api-tool <command> <mod id> [arguments] [options]
  nexus-api-tool <command> --list <file> [options]       a query or download command for every line of <file>
  nexus-api-tool --usage [--verbose] [--per <unit>]      requests made, and what is left of the API limits
  nexus-api-tool --help [--verbose]

COMMANDS                                                                                  API USED
  desc <mod>                          name, author, version, summary and description      v1
                                      (BBCode as stored; --plain gives readable text)
  changelog <mod>                     the changelog, per version (--plain)                v1
  files <mod>                         every file that isn't archived, grouped by category GraphQL
                                      (main, update, optional, miscellaneous, old)
  archived-files <mod>                the same for archived and removed files             GraphQL
  file-note <mod> <file#>             the note on one file (--plain)                      GraphQL
  contents <mod> <file#>              the files inside one archive, its plugins and BSAs  GraphQL (v1 + content
                                                                                          preview if not indexed)
  dl-file <mod> <file#> --dest <path> download one file; <path> is a folder or a file name v1 + file download
                                      Needs a Premium account. --force replaces an existing file,
                                      --skip-existing leaves it, --max-size <size> skips bigger files
  permissions <mod or address>        the page's Permissions and credits section          mod page (website;
                                      If Nexus turns the program away, --html <saved     not an API; ours
                                      page> reads a page saved from a browser             only)
  reqs <mod>                          what this mod requires                              GraphQL
  search-files --ext .esp [--mod-range A-B] [--name <wildcard>] [--max-results n]        GraphQL
                                      every archive file with that extension or name,
                                      in mods A to B (ours only). 10,000 results by default
  dependents <mod>                    the mods that require this mod                      GraphQL
  list-mods <user>                    the mods a user uploaded, newest first              GraphQL
                                      reqs, dependents and list-mods return 50 entries by default,
                                      at most 200 per call; use next_offset from the output with --offset

  save-desc <mod> --dest <path>       save the description page as a BBCode text file     v3
  set-desc <mod> --file <path>        REPLACE the description page with a BBCode text     v3
                                      file. The current page is backed up first; if that fails nothing is
                                      changed. Options: --backup-dir <folder>, --dry-run, --allow-html
  upload-file <mod> --file <path> --name <label> --file-version <v>                       v3 + Cloudflare-
                                      upload an archive as a new file (under 100 MiB).    limited upload
                                      Options: --replaces <file#> (new version of that file, the old one
                                      is archived), --category main|optional|miscellaneous, --note <text>,
                                      --changelog <text>, --set-mod-version, --dry-run, --allow-html

OPTIONS
  --game <domain|id>  the game, as its Nexus domain (skyrim, skyrimspecialedition) or its number. Default:
                      the NEXUS_GAME_ID environment variable if it is set, else skyrimspecialedition
  --limit <n>         reqs, dependents, list-mods: entries to return (default 50, at most 200)
  --offset <n>        reqs, dependents, list-mods: entries to skip (default 0)
  --list <file>       run the command once per line of <file>. A line holds what the command takes
                      (a mod id, a mod id and a file number, or a user name), separated by spaces or
                      commas; # starts a comment. For desc, changelog, files, archived-files, file-note,
                      contents, dl-file, reqs, dependents, list-mods and save-desc. Prints one JSON
                      object per line, and stops at the first rate limit.
  --plain             desc, changelog, file-note: readable text instead of BBCode. Links become
                      text (url), list items - item, headings and spoiler titles their own lines;
                      images are dropped, videos become [video: url], invisible characters (byte-order
                      marks, zero-width spaces) are removed and runs of blank lines collapse to one
  --interval <s>      seconds between two requests of the same kind, instead of the built-in pacing
                      (0 = no pause). Applies to every kind this run makes.
  --usage             (also --rate, --rates, --limits, or --limit with no command) report the requests
                      made and what is left of the API limits. --verbose adds a breakdown by command,
                      --per s|m|h|d sets the window (second, minute, hour, day; default hour)
  --verbose           with --help: the calls each command makes and how many runs the limits allow
  --help, --version

RATE LIMITS
  Nexus Mods API: 2000/hr per API key (v1 and v3 are counted separately), and 20,000/day.
    2000/hr = 0.56/sec, 33.3/min, 2,000/hr, 48,000/day; the daily limit of 20,000 comes first.
  GraphQL: Nexus Mods publishes no limit and reports none.
  Built-in pacing: queries 1 per 3 s, downloads 1 per 4 s, uploads 1 per 31 s (--interval overrides it).
  Cloudflare's upload limit is inconsistent with their stated instructions e.g. "retry_after: 30". Once this
  limit is tripped Cloudflare may impose a limit of 4/hour.

API KEY
  Every command that uses v1 or v3 reads your personal API key from the NEXUS_API_KEY environment
  variable (nexusmods.com > Site preferences > API Keys). GraphQL needs no key.

FILES
  nexus-api-tool-usage.jsonl and nexus-api-tool-state.json beside the exe (or in %LOCALAPPDATA%\nexus-api-tool
  when that folder can't be written): every request made, and the last limits Nexus Mods reported.

EXIT CODES
  0 ok   1 the request failed   2 bad arguments   3 rate limited (the error says whether by Nexus Mods
  or by Cloudflare, and how long to wait)
)";

struct Failure : std::runtime_error {
    int code;
    Failure(int exit_code, const std::string& message) : std::runtime_error(message), code(exit_code) {}
};

// Exit code 3: Nexus Mods or Cloudflare is refusing requests for now. by says which; retry_after is in seconds.
struct RateLimited : Failure {
    std::string by;
    long long retry_after;
    RateLimited(const std::string& who, long long seconds, const std::string& message) : Failure(3, message), by(who), retry_after(seconds) {}
};

[[noreturn]] void fail(const std::string& message) { throw Failure(1, message); }
[[noreturn]] void bad_args(const std::string& message) { throw Failure(2, message + " (see --help)"); }

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------- API key

std::string api_key() {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(kKeyEnvVar, buf, 512);
    if (n > 0 && n < 512) return to_utf8(buf);
    // A variable set after this shell started is only in the registry so far.
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", kKeyEnvVar, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return to_utf8(buf);
    fail(std::string("no API key: set the ") + kKeyEnvVarText +
         " environment variable to your personal key (nexusmods.com > Site preferences > API Keys)");
}

// ---------------------------------------------------------------- HTTP

struct HttpCloser {
    void operator()(void* h) const {
        if (h) WinHttpCloseHandle(h);
    }
};
using HttpHandle = std::unique_ptr<void, HttpCloser>;

// CDN links can hold spaces and non-ASCII names. Existing %XX, '?' and the like stay as they are.
std::string encode_url(const std::string& url) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7F || std::string("\"<>\\^`{|}").find(static_cast<char>(c)) != std::string::npos) {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

struct Http {
    int status = 0;
    std::string error_body;                      // the response body when status isn't 200
    std::map<std::string, std::string> headers;  // lower-case names: x-rl-*, retry-after, server, cf-ray
};

// What a WinHTTP error code means, in words.
std::string connection_problem(DWORD code, const std::wstring& host) {
    std::string h = to_utf8(host);
    switch (code) {
        case 12007: return "could not find " + h + ". Is the computer online?";
        case 12029: return "could not connect to " + h + ". Is the computer online, or is a firewall blocking nexus-api-tool.exe?";
        case 12002: return h + " took too long to answer (the request timed out). Try again in a few minutes.";
        case 12030: return "the connection to " + h + " was cut off. Try again in a few minutes.";
        case 12175: return "the secure connection to " + h + " failed (certificate or TLS problem).";
        default: return "the request to " + h + " failed (WinHTTP error " + std::to_string(code) + ").";
    }
}

// Sends one request. sink receives the body of a 200 response, piece by piece.
Http send(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
          const std::string& body, const std::function<void(const char*, size_t)>& sink) {
    std::wstring wurl = to_wide(encode_url(url));
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) fail("bad URL from the server");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    std::wstring agent = to_wide(std::string(kToolName) + "/" + kToolVersion);
    HttpHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) fail("could not start WinHTTP");
    WinHttpSetTimeouts(session.get(), 30000, 30000, 60000, 120000);
    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) fail("could not connect to the server");
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request(WinHttpOpenRequest(connection.get(), to_wide(method).c_str(), path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) fail("could not create the request");
    for (const auto& h : headers) {
        std::wstring wh = to_wide(h);
        WinHttpAddRequestHeaders(request.get(), wh.c_str(), static_cast<DWORD>(wh.size()), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        fail(connection_problem(GetLastError(), host));

    Http result;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    DWORD raw_size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &raw_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && raw_size > 0) {
        std::wstring raw(raw_size / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            std::string text = to_utf8(raw.c_str());
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find("\r\n", start);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(start, end - start);
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string name = lower(line.substr(0, colon));
                    std::string value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    if (name.rfind("x-rl-", 0) == 0 || name == "retry-after" || name == "server" || name == "cf-ray")
                        result.headers[name] = value;
                }
                start = end + 2;
            }
        }
    }

    std::vector<char> buffer(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got))
            fail(connection_problem(GetLastError(), host));
        if (got == 0) break;
        if (result.status >= 200 && result.status < 300)
            sink(buffer.data(), got);
        else if (result.error_body.size() < 4096)
            result.error_body.append(buffer.data(), got);
    }
    return result;
}

// What Nexus Mods said, in words. 429 never gets here: api_send turns it into RateLimited.
std::string api_error(int status, const std::string& body) {
    std::string detail;
    json j = json::parse(body, nullptr, false);
    if (j.is_object()) {
        for (const char* k : {"error", "message", "title", "detail"})  // the last one found is the most specific
            if (j.contains(k) && j[k].is_string()) detail = j[k].get<std::string>();
    }
    std::string said = detail.empty() ? "" : " Nexus Mods said: " + detail;
    if (status == 401)
        return std::string("Nexus Mods refused the API key. Check that the ") + kKeyEnvVarText +
               " environment variable holds your personal API key (nexusmods.com > Site preferences > API Keys).";
    if (status == 403) return "Nexus Mods refused this request (HTTP 403)." + said;
    if (status == 404) return "Nexus Mods couldn't find that (HTTP 404)." + said;
    if (status >= 500) return "Nexus Mods' server ran into a problem (HTTP " + std::to_string(status) + "). Try again in a few minutes." + said;
    return "Nexus Mods answered with HTTP " + std::to_string(status) + "." + said;
}

// ---------------------------------------------------------------- pacing, usage log and rate limits
//
// Every request goes through api_send. Requests of one kind are spaced out (queries 1 per 3 s, downloads 1 per 4 s,
// uploads 1 per 31 s; --interval overrides it), across separate runs too: the time of the last request of each kind
// and the last limits Nexus reported are kept in nexus-api-tool-state.json, and every request is logged to
// nexus-api-tool-usage.jsonl. Both live beside the exe, or in %LOCALAPPDATA%\nexus-api-tool when that folder
// can't be written. --usage reads them.

enum class Api { V1, GraphQl, V3, Download, Preview, Storage };
enum class Kind { Query, Download, Upload };

const char* api_name(Api a) {
    switch (a) {
        case Api::V1: return "v1";
        case Api::GraphQl: return "graphql";
        case Api::V3: return "v3";
        case Api::Download: return "file download";
        case Api::Preview: return "content preview";
        default: return "upload storage";
    }
}

const char* kind_name(Kind k) { return k == Kind::Query ? "query" : k == Kind::Download ? "download" : "upload"; }

constexpr long long kQueryGapMs = 3000, kDownloadGapMs = 4000, kUploadGapMs = 31000;
constexpr long long kHourlyLimit = 2000, kDailyLimit = 20000;

struct RunContext {
    std::string command;      // the subcommand running now, for the usage log
    long long gap_ms = -1;    // --interval, or -1 for the built-in pacing
} g_run;

long long gap_ms(Kind k) {
    if (g_run.gap_ms >= 0) return g_run.gap_ms;
    return k == Kind::Query ? kQueryGapMs : k == Kind::Download ? kDownloadGapMs : kUploadGapMs;
}

long long now_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

std::string iso_ms(long long ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string clock_utc(long long ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%H:%M UTC on %Y-%m-%d", &utc);
    return buffer;
}

// "2026-11-12 17:00:00 +0000" (the x-rl-*-reset headers) -> ms since 1970, 0 if it doesn't parse.
long long parse_reset(const std::string& s) {
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<long long>(_mkgmtime(&tm)) * 1000;
}

std::string with_commas(long long n) {
    std::string s = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return n < 0 ? "-" + out : out;
}

std::string read_file(const fs::path& path);

fs::path data_dir() {
    static fs::path dir;
    if (!dir.empty()) return dir;
    auto writable = [](const fs::path& d) {
        fs::path probe = d / L"nexus-api-tool-usage.jsonl";
        HANDLE h = CreateFileW(probe.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    };
    std::wstring exe(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    fs::path beside = fs::path(exe.substr(0, n)).parent_path();
    if (n > 0 && writable(beside)) {
        dir = beside;
    } else {
        wchar_t buf[32768];
        DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
        fs::path local = (m > 0 && m < 32768) ? fs::path(buf) / L"nexus-api-tool" : fs::temp_directory_path() / L"nexus-api-tool";
        std::error_code ec;
        fs::create_directories(local, ec);
        dir = local;
    }
    return dir;
}

fs::path usage_path() { return data_dir() / L"nexus-api-tool-usage.jsonl"; }
fs::path state_path() { return data_dir() / L"nexus-api-tool-state.json"; }

// Several copies of the tool can run at once; this serialises their pacing and state updates.
struct StateLock {
    HANDLE h = INVALID_HANDLE_VALUE;
    StateLock() {
        fs::path p = data_dir() / L"nexus-api-tool.lock";
        for (int i = 0; i < 1500; ++i) {
            h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) return;
            Sleep(100);
        }
        fail("another copy of nexus-api-tool has held " + p.u8string() + " for over two minutes. If none is running, delete that file.");
    }
    ~StateLock() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
};

json load_state() {
    std::error_code ec;
    if (!fs::exists(state_path(), ec)) return json::object();
    try {
        json j = json::parse(read_file(state_path()), nullptr, false);
        return j.is_object() ? j : json::object();
    } catch (const std::exception&) {
        return json::object();
    }
}

void save_state(const json& s) {
    fs::path tmp = state_path();
    tmp += L".tmp";
    std::string text = s.dump(1);
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;  // pacing state is a convenience; never fail a request over it
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) MoveFileExW(tmp.c_str(), state_path().c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Keeps the log to the last 8 days once it passes 8 MB.
void prune_usage() {
    try {
        std::string text = read_file(usage_path()), kept;
        long long cutoff = now_ms() - 8LL * 24 * 3600 * 1000;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            json j = json::parse(line, nullptr, false);
            if (j.is_object() && j.value("t", 0LL) >= cutoff) kept += line + "\n";
            start = end + 1;
        }
        fs::path tmp = usage_path();
        tmp += L".tmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return;
        std::fwrite(kept.data(), 1, kept.size(), f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), usage_path().c_str(), MOVEFILE_REPLACE_EXISTING);
    } catch (const std::exception&) {
    }
}

void log_usage(const json& entry) {
    std::string line = entry.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    HANDLE h = CreateFileW(usage_path().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (size.QuadPart > 8LL * 1024 * 1024) prune_usage();
}

long long header_number(const Http& r, const char* name) {
    auto it = r.headers.find(name);
    return it == r.headers.end() ? -1 : std::strtoll(it->second.c_str(), nullptr, 10);
}

// The limits Nexus reported on this response, or null when it sent none (GraphQL never does).
json limits_from(const Http& r) {
    if (header_number(r, "x-rl-hourly-remaining") < 0) return nullptr;
    auto text = [&](const char* n) { auto it = r.headers.find(n); return it == r.headers.end() ? std::string() : it->second; };
    return {{"hourly_limit", header_number(r, "x-rl-hourly-limit")},
            {"hourly_remaining", header_number(r, "x-rl-hourly-remaining")},
            {"hourly_reset", parse_reset(text("x-rl-hourly-reset"))},
            {"daily_limit", header_number(r, "x-rl-daily-limit")},
            {"daily_remaining", header_number(r, "x-rl-daily-remaining")},
            {"daily_reset", parse_reset(text("x-rl-daily-reset"))},
            {"as_of", now_ms()}};
}

RateLimited nexus_limit(Api api, const json& l) {
    long long now = now_ms();
    bool hour_out = l.value("hourly_remaining", 1LL) <= 0 && l.value("hourly_reset", 0LL) > now;
    long long reset = hour_out ? l.value("hourly_reset", 0LL) : l.value("daily_reset", 0LL);
    long long wait = reset > now ? (reset - now + 999) / 1000 : 60;
    std::string msg = std::string("Nexus Mods' API limit for this key is used up (") + api_name(api) + " API): " +
                      with_commas(std::max(0LL, l.value("hourly_remaining", 0LL))) + " of " + with_commas(l.value("hourly_limit", kHourlyLimit)) +
                      " requests left this hour and " + with_commas(std::max(0LL, l.value("daily_remaining", 0LL))) + " of " +
                      with_commas(l.value("daily_limit", kDailyLimit)) + " left today. ";
    msg += reset > now ? "Try again after " + clock_utc(reset) + "." : "Try again later.";
    return RateLimited("Nexus Mods", wait, msg);
}

RateLimited cloudflare_limit(Kind kind, const Http& r) {
    long long wait = header_number(r, "retry-after");
    json j = json::parse(r.error_body, nullptr, false);
    if (j.is_object() && j.contains("retry_after") && j["retry_after"].is_number()) wait = j["retry_after"].get<long long>();
    if (wait <= 0) wait = 30;
    std::string msg = "Cloudflare, the network service in front of Nexus Mods, is refusing requests from this program for now "
                      "(HTTP 429, Cloudflare error 1015). This is not your Nexus Mods API limit. ";
    if (kind == Kind::Upload)
        msg += "Cloudflare asks to wait " + std::to_string(wait) + " seconds, but its upload limit doesn't keep to that: "
               "once it has been tripped, Cloudflare may allow as few as 4 uploads an hour.";
    else
        msg += "Cloudflare asks to wait " + std::to_string(wait) + " seconds before trying again.";
    return RateLimited("Cloudflare", wait, msg);
}

bool from_cloudflare(const Http& r) {
    std::string body = lower(r.error_body);
    return body.find("1015") != std::string::npos || body.find("cloudflare") != std::string::npos;
}

Kind kind_of(Api api, const std::string& method, const std::string& url) {
    if (api == Api::Download) return Kind::Download;
    if (api == Api::V3 && method == "POST" && url.size() >= 8 && url.compare(url.size() - 8, 8, "/uploads") == 0) return Kind::Upload;
    return Kind::Query;
}

// Sends one request with pacing, logging and rate-limit checks. Storage is the upload's own presigned PUT, which
// belongs to the upload just started and isn't paced.
Http api_send(Api api, const std::string& method, const std::string& url, const std::vector<std::string>& headers,
              const std::string& body, const std::function<void(const char*, size_t)>& sink) {
    Kind kind = kind_of(api, method, url);
    bool counted = api == Api::V1 || api == Api::V3;
    if (api != Api::Storage) {
        StateLock lock;
        json state = load_state();
        if (counted && state.contains("limits") && state["limits"].contains(api_name(api))) {
            const json& l = state["limits"][api_name(api)];
            long long now = now_ms();
            if ((l.value("hourly_remaining", 1LL) <= 0 && l.value("hourly_reset", 0LL) > now) ||
                (l.value("daily_remaining", 1LL) <= 0 && l.value("daily_reset", 0LL) > now))
                throw nexus_limit(api, l);
        }
        long long last = state.contains("last") ? state["last"].value(kind_name(kind), 0LL) : 0LL;
        long long wait = last + gap_ms(kind) - now_ms();
        if (wait > 0) Sleep(static_cast<DWORD>(std::min(wait, 600000LL)));
        state["last"][kind_name(kind)] = now_ms();
        save_state(state);
    }

    long long started = now_ms();
    Http r = send(method, url, headers, body, sink);

    std::string path = url;
    size_t q = path.find('?');
    if (q != std::string::npos) path.erase(q);  // download links carry signed query strings
    json entry = {{"t", started}, {"command", g_run.command}, {"api", api_name(api)}, {"kind", kind_name(kind)}, {"method", method},
                  {"url", path}, {"status", r.status}};
    json limits = limits_from(r);
    bool throttled = r.status == 429;
    std::string by = throttled ? (from_cloudflare(r) ? "Cloudflare" : "Nexus Mods") : "";
    if (!limits.is_null()) entry["limits"] = limits;
    if (throttled) entry["rate_limited_by"] = by;
    log_usage(entry);
    if (!limits.is_null() || throttled) {
        StateLock lock;
        json state = load_state();
        if (!limits.is_null()) state["limits"][api_name(api)] = limits;
        if (throttled && by == "Cloudflare") state["cloudflare_limited"] = {{"at", started}, {"kind", kind_name(kind)}};
        save_state(state);
    }

    if (throttled) {
        if (by == "Cloudflare") throw cloudflare_limit(kind, r);
        throw nexus_limit(api, limits.is_null() ? json{{"hourly_remaining", 0}, {"daily_remaining", 0}} : limits);
    }
    return r;
}

std::vector<std::string> nexus_headers(bool json_body) {
    std::vector<std::string> h = {"apikey: " + api_key(), "Accept: application/json", std::string("Application-Name: ") + kToolName,
                                  std::string("Application-Version: ") + kToolVersion};
    if (json_body) h.push_back("Content-Type: application/json");
    return h;
}

json parse_reply(const std::string& text, const char* what) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) fail(std::string("Nexus Mods sent a reply this program can't read (") + what + " API).");
    return j;
}

json v1_get(const std::string& path) {
    std::string text;
    Http r = api_send(Api::V1, "GET", std::string(kV1) + path, nexus_headers(false), "", [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status != 200) fail(api_error(r.status, r.error_body));
    return parse_reply(text, "v1");
}

// v3 is the API that can change things. Returns null for a response with no body.
json v3_call(const std::string& method, const std::string& path, const json* body = nullptr) {
    std::string text;
    Http r = api_send(Api::V3, method, std::string(kV3) + path, nexus_headers(body != nullptr),
                      body ? body->dump(-1, ' ', false, json::error_handler_t::replace) : "", [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status < 200 || r.status >= 300) fail(api_error(r.status, r.error_body));
    return text.empty() ? json(nullptr) : parse_reply(text, "v3");
}

json graphql(const std::string& query, const json& variables) {
    json payload = {{"query", query}, {"variables", variables}};
    std::string text;
    Http r = api_send(Api::GraphQl, "POST", kGraphQl, {"Content-Type: application/json", "Accept: application/json",
                                                       std::string("Application-Name: ") + kToolName, std::string("Application-Version: ") + kToolVersion},
                      payload.dump(), [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status != 200) fail(api_error(r.status, r.error_body));
    json j = parse_reply(text, "GraphQL");
    if (j.contains("errors") && j["errors"].is_array() && !j["errors"].empty()) {
        std::string messages;
        for (const auto& e : j["errors"])
            if (e.contains("message") && e["message"].is_string()) messages += (messages.empty() ? "" : "; ") + e["message"].get<std::string>();
        fail("Nexus Mods' GraphQL API reported an error: " + (messages.empty() ? j["errors"].dump() : messages));
    }
    return j["data"];
}

// ---------------------------------------------------------------- JSON and text helpers

std::string text_of(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

json value_of(const json& j, const char* key) { return j.contains(key) ? j[key] : json(nullptr); }

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 63));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    }
}

// The API returns text with <br /> line breaks and HTML entities. BBCode tags are left alone.
std::string clean_text(const std::string& s) {
    static const std::map<std::string, char> named = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
    std::string out;
    out.reserve(s.size());
    bool after_break = false;  // the last thing written was a <br />
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '<' && lower(s.substr(i, 3)) == "<br") {
            size_t end = s.find('>', i);
            if (end == std::string::npos) {
                out += s.substr(i);
                break;
            }
            if (out.empty() || out.back() != '\n' || after_break) out += '\n';  // the site stores "\n<br />" for one break
            after_break = true;
            i = end + 1;
            continue;
        }
        after_break = false;
        if (c == '&') {
            size_t end = s.find(';', i);
            std::string name = end != std::string::npos && end - i <= 10 ? s.substr(i + 1, end - i - 1) : std::string();
            auto it = named.find(name);
            if (it != named.end()) {
                out += it->second;
                i = end + 1;
            } else if (name.size() > 1 && name[0] == '#') {
                bool is_hex = name[1] == 'x' || name[1] == 'X';
                append_utf8(out, std::strtoul(name.c_str() + (is_hex ? 2 : 1), nullptr, is_hex ? 16 : 10));
                i = end + 1;
            } else {
                out += c;
                ++i;
            }
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::string strip_bbcode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') {
            size_t end = s.find(']', i);
            if (end != std::string::npos && end - i <= 200) {
                i = end;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// --plain: BBCode (after clean_text) as readable text. Only known tag names count as tags, so "[WIP]" or "[SSE]"
// in the text stays. Links become "text (url)", list items "- item", headings and spoiler titles their own lines,
// images are dropped, videos become "[video: url]", byte-order marks and zero-width characters are removed.
std::string plain_text(const std::string& s) {
    static const std::set<std::string> known = {
        "b", "i", "u", "s", "size", "color", "colour", "font", "center", "centre", "left", "right", "justify", "align",
        "quote", "code", "spoiler", "heading", "line", "hr", "list", "ul", "ol", "*", "li", "url", "email", "img",
        "youtube", "video", "table", "tr", "td", "th", "sup", "sub", "indent", "highlight", "style", "strike"};
    static const std::set<std::string> blocks = {"center", "centre", "left", "right", "justify", "align", "quote", "code",
                                                 "heading", "table", "tr", "indent"};
    auto newline = [](std::string& out) {
        if (!out.empty() && out.back() != '\n') out += '\n';
    };
    std::string out;
    std::vector<std::pair<size_t, std::string>> links;  // where each open [url] began in out, and its address
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '[') {
            out += s[i++];
            continue;
        }
        size_t end = s.find(']', i);
        if (end == std::string::npos) {
            out += s.substr(i);
            break;
        }
        std::string inner = s.substr(i + 1, end - i - 1);
        bool closing = !inner.empty() && inner[0] == '/';
        if (closing) inner.erase(0, 1);
        size_t eq = inner.find('=');
        std::string name = lower(inner.substr(0, std::min(eq, inner.find(' '))));
        std::string arg = eq == std::string::npos ? std::string() : inner.substr(eq + 1);
        if (arg.size() >= 2 && (arg.front() == '"' || arg.front() == '\'') && arg.back() == arg.front()) arg = arg.substr(1, arg.size() - 2);
        if (!known.count(name)) {
            out += s[i++];
            continue;
        }
        i = end + 1;
        auto content_until_close = [&](const std::string& tag) {  // the raw text up to [/tag]; skips past it
            std::string rest = lower(s.substr(i));
            size_t close = rest.find("[/" + tag + "]");
            std::string content = close == std::string::npos ? s.substr(i) : s.substr(i, close);
            i = close == std::string::npos ? s.size() : i + close + tag.size() + 3;
            return content;
        };
        if (!closing && name == "img") {
            content_until_close("img");
        } else if (!closing && (name == "youtube" || name == "video")) {
            std::string id = content_until_close(name);
            while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back()))) id.pop_back();
            while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front()))) id.erase(0, 1);
            if (!id.empty())
                out += "[video: " + (id.find("://") == std::string::npos ? "https://www.youtube.com/watch?v=" + id : id) + "]";
        } else if (name == "url" || name == "email") {
            if (!closing) {
                links.push_back({out.size(), arg});
            } else if (!links.empty()) {
                auto [start, address] = links.back();
                links.pop_back();
                std::string text = out.substr(start);
                out.resize(start);
                std::string trimmed = text;
                while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
                while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(0, 1);
                if (address.empty() || trimmed == address) out += text;
                else if (trimmed.empty()) out += address;
                else out += trimmed + " (" + address + ")";
            }
        } else if (name == "*" || name == "li") {
            if (!closing) {
                newline(out);
                out += "- ";
            }
        } else if (name == "list" || name == "ul" || name == "ol") {
            newline(out);
        } else if (name == "line" || name == "hr") {
            newline(out);
            out += '\n';
        } else if (name == "spoiler") {
            newline(out);
            if (!closing && !arg.empty()) out += arg + "\n";
        } else if (name == "td" || name == "th") {
            if (closing) out += "  ";
        } else if (blocks.count(name)) {
            newline(out);
        }
    }
    // Byte-order marks, zero-width spaces and joiners, word joiners: invisible, and they trip some consoles.
    for (const char* junk : {"\xEF\xBB\xBF", "\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", "\xE2\x81\xA0"})
        for (size_t p; (p = out.find(junk)) != std::string::npos;) out.erase(p, std::strlen(junk));
    // Trailing spaces on each line, at most one blank line in a row, no blank lines at the start or end.
    std::string tidy;
    size_t blank = 0;
    for (size_t start = 0; start <= out.size();) {
        size_t nl = out.find('\n', start);
        std::string line = out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) {
            ++blank;
        } else {
            if (!tidy.empty()) tidy += blank > 0 ? "\n\n" : "\n";
            tidy += line;
            blank = 0;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return tidy;
}

// ---------------------------------------------------------------- commands

std::string read_file(const fs::path& path);

struct Target {
    std::string game;
    std::string mod;
    int limit = kDefaultLimit;
    int offset = 0;

    std::string v1_mod() const { return "/games/" + game + "/mods/" + mod; }
    std::string page() const { return "https://www.nexusmods.com/" + game + "/mods/" + mod; }
    long long mod_id() const { return std::stoll(mod); }
    json header() const { return {{"mod_id", mod_id()}, {"game", game}}; }
};

// The category keys, in the order the page shows them. Nexus has no name for category 6 (removed files).
std::string category_key(int id) {
    switch (id) {
        case 1: return "main";
        case 2: return "update";
        case 3: return "optional";
        case 4: return "old";
        case 5: return "miscellaneous";
        case 6: return "removed";
        case 7: return "archived";
        default: return "category_" + std::to_string(id);
    }
}

bool is_archived(int category) { return category == 6 || category == 7; }

std::string iso_time(long long timestamp) {
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000+00:00", &utc);
    return buffer;
}

// One object from the GraphQL modFiles list.
json file_object(const json& f) {
    int category = f.value("categoryId", 0);
    long long uploaded = f.value("date", 0LL);
    std::string uri = text_of(f, "uri");
    std::string bytes = text_of(f, "sizeInBytes");
    return {{"file_id", value_of(f, "fileId")},
            {"label", clean_text(text_of(f, "name"))},
            {"version", value_of(f, "version")},
            {"category", category_key(category)},
            {"uploaded", iso_time(uploaded)},
            {"uploaded_timestamp", uploaded},
            {"note", clean_text(text_of(f, "description"))},
            {"file_name", uri.empty() || uri.find('/') != std::string::npos ? json(nullptr) : json(uri)},  // newer uploads hold a storage path here
            {"size_bytes", bytes.empty() ? json(nullptr) : json(std::stoll(bytes))},
            {"size_kb", value_of(f, "size")},
            {"primary", f.value("primary", 0) != 0}};
}

std::string game_number(const std::string& game) {
    if (game == kDefaultGame) return "1704";  // saves a request in the usual case
    try {
        return std::to_string(graphql("query($d:String!){game(domainName:$d){id}}", {{"d", game}})["game"]["id"].get<long long>());
    } catch (const Failure&) {
        fail("Nexus has no game with the domain " + game);
    }
}

// Every file of a mod, whatever its category, in one request. Unlike v1 this also works on unpublished mods.
json mod_files(const Target& t) {
    json files = graphql("query($m:ID!,$g:ID!){modFiles(modId:$m,gameId:$g){fileId name version categoryId description size sizeInBytes date primary uri}}",
                         {{"m", t.mod}, {"g", game_number(t.game)}})["modFiles"];
    if (files.empty()) {  // a mod with no files and a mod that doesn't exist look the same here
        json nodes = graphql("query($ids:[CompositeDomainWithIdInput!]!){legacyModsByDomain(ids:$ids){nodes{modId}}}",
                             {{"ids", json::array({{{"gameDomain", t.game}, {"modId", t.mod_id()}}})}})["legacyModsByDomain"]["nodes"];
        if (nodes.empty()) fail("Nexus Mods has no mod " + t.mod + " for the game " + t.game + ".");
    }
    return files;
}

json cmd_desc(const Target& t, bool plain) {
    auto text = [&](const std::string& s) { return plain ? plain_text(clean_text(s)) : clean_text(s); };
    json m = v1_get(t.v1_mod() + ".json");
    json out = t.header();
    out["name"] = value_of(m, "name");
    out["author"] = value_of(m, "author");
    out["uploader"] = value_of(m, "uploaded_by");
    out["version"] = value_of(m, "version");
    out["status"] = value_of(m, "status");
    out["updated"] = value_of(m, "updated_time");
    out["summary"] = text(text_of(m, "summary"));
    out["description"] = text(text_of(m, "description"));
    return out;
}

json cmd_changelog(const Target& t, bool plain) {
    json log = v1_get(t.v1_mod() + "/changelogs.json");
    json versions = json::array();
    if (log.is_object()) {
        for (auto& [version, entries] : log.items()) {
            json lines = json::array();
            for (const auto& e : entries)
                if (e.is_string()) lines.push_back(plain ? plain_text(clean_text(e.get<std::string>())) : clean_text(e.get<std::string>()));
            versions.push_back({{"version", version}, {"entries", lines}});
        }
    }
    json out = t.header();
    out["changelog"] = versions;
    return out;
}

json cmd_files(const Target& t, bool archived) {
    json listing = mod_files(t);
    std::map<int, std::vector<json>> groups;
    for (const auto& f : listing) {
        int category = f.value("categoryId", 0);
        if (is_archived(category) == archived) groups[category].push_back(f);
    }
    std::vector<int> order = archived ? std::vector<int>{7, 6} : std::vector<int>{1, 2, 3, 5, 4};
    for (const auto& g : groups)
        if (std::find(order.begin(), order.end(), g.first) == order.end()) order.push_back(g.first);

    json categories = json::object();
    size_t total = 0;
    for (int id : order) {
        auto& files = groups[id];
        std::stable_sort(files.begin(), files.end(), [](const json& a, const json& b) {
            return a.value("date", 0LL) > b.value("date", 0LL);
        });
        json list = json::array();
        for (const auto& f : files) list.push_back(file_object(f));
        total += list.size();
        categories[category_key(id)] = list;
    }
    json out = t.header();
    out["count"] = total;
    out["categories"] = categories;
    return out;
}

json cmd_file_note(const Target& t, const std::string& file, bool plain) {
    json files = mod_files(t);
    auto f = std::find_if(files.begin(), files.end(), [&](const json& x) { return x.value("fileId", 0LL) == std::stoll(file); });
    if (f == files.end()) fail("mod " + t.mod + " has no file " + file);
    json o = file_object(*f);
    json out = t.header();
    for (const char* k : {"file_id", "label", "version", "category", "uploaded", "note"}) out[k] = o[k];
    if (plain) out["note"] = plain_text(o["note"].get<std::string>());
    return out;
}

struct DownloadOptions {
    std::string dest;
    bool force = false;
    bool skip_existing = false;  // a file already at the target is left alone and reported as skipped
    long long max_size = -1;     // bytes; a larger file is skipped before anything is downloaded
};

json cmd_dl_file(const Target& t, const std::string& file, const DownloadOptions& o) {
    std::string base = t.v1_mod() + "/files/" + file;
    json info = v1_get(base + ".json");
    std::string file_name = fs::u8path(text_of(info, "file_name")).filename().u8string();
    if (file_name.empty()) fail("Nexus Mods gave no file name for file " + file + " of mod " + t.mod + ".");
    long long size = info.contains("size_in_bytes") && info["size_in_bytes"].is_number_integer() ? info["size_in_bytes"].get<long long>() : -1;

    fs::path target = to_wide(o.dest);
    bool is_folder = fs::is_directory(target) || o.dest.back() == '/' || o.dest.back() == '\\';
    if (is_folder) target /= fs::u8path(file_name);

    json out = t.header();
    out["file_id"] = value_of(info, "file_id");
    out["file_name"] = file_name;  // the name Nexus has for the file, also when the file list shows none
    out["saved_as"] = target.filename().u8string();
    out["path"] = fs::absolute(target).u8string();
    out["size_bytes"] = size < 0 ? json(nullptr) : json(size);
    if (fs::exists(target) && o.skip_existing) {
        out["skipped"] = true;
        out["reason"] = "the file is already there (--skip-existing)";
        return out;
    }
    if (o.max_size >= 0 && size > o.max_size) {
        out["skipped"] = true;
        out["reason"] = "the file is " + with_commas(size) + " bytes, more than --max-size (" + with_commas(o.max_size) + ")";
        return out;
    }
    if (fs::exists(target) && !o.force) fail(target.u8string() + " already exists. --force replaces it; --skip-existing leaves it.");

    json links;
    {
        std::string text;
        Http r = api_send(Api::V1, "GET", std::string(kV1) + base + "/download_link.json", nexus_headers(false), "",
                          [&](const char* p, size_t n) { text.append(p, n); });
        if (r.status == 403)
            fail("Nexus Mods won't give this key a download link: downloads through the API need a Premium account. " + api_error(r.status, r.error_body));
        if (r.status != 200) fail(api_error(r.status, r.error_body));
        links = parse_reply(text, "v1");
    }
    if (!links.is_array() || links.empty() || !links[0].contains("URI")) fail("Nexus Mods returned no download link for file " + file + ".");

    if (target.has_parent_path()) fs::create_directories(target.parent_path());
    fs::path part = target;
    part += L".part";

    FILE* out_file = _wfopen(part.c_str(), L"wb");
    if (!out_file) fail("cannot write " + part.u8string());
    long long written = 0;
    try {
        Http r = api_send(Api::Download, "GET", links[0]["URI"].get<std::string>(), {}, "", [&](const char* p, size_t n) {
            if (std::fwrite(p, 1, n, out_file) != n) fail("cannot write " + part.u8string() + " (is the disk full?)");
            written += static_cast<long long>(n);
        });
        std::fclose(out_file);
        out_file = nullptr;
        if (r.status != 200) fail("the download failed: the file server answered with HTTP " + std::to_string(r.status) + ".");
        if (size >= 0 && size != written)
            fail("the download stopped early: got " + with_commas(written) + " of " + with_commas(size) + " bytes.");
        fs::rename(part, target);
    } catch (...) {
        if (out_file) std::fclose(out_file);
        std::error_code ignored;
        fs::remove(part, ignored);
        throw;
    }
    out["skipped"] = false;
    out["bytes"] = written;
    return out;
}

// ---------------------------------------------------------------- contents

long long id_number(const json& v);

// The files inside one archive. GraphQL's file-contents index first; files it hasn't indexed (often the newest)
// fall back to the content preview Nexus makes for every upload (its link comes from the v1 API).
void collect_preview(const json& node, json& files) {
    if (!node.is_object()) return;
    if (node.contains("files") && node["files"].is_array()) {  // the newer manifest: a flat list
        for (const auto& f : node["files"])
            if (f.is_object()) files.push_back({{"path", value_of(f, "file_path")}, {"size", value_of(f, "file_size")}});
        return;
    }
    if (node.value("type", "") == "file") {  // the older preview: a folder tree
        files.push_back({{"path", value_of(node, "path")}, {"size", value_of(node, "size")}});
        return;
    }
    if (node.contains("children") && node["children"].is_array())
        for (const auto& c : node["children"]) collect_preview(c, files);
}

json cmd_contents(const Target& t, const std::string& file) {
    std::string game_id = game_number(t.game);
    json filter = {{"fileId", json::array({{{"value", std::stoll(file)}, {"op", "EQUALS"}}})},
                   {"gameId", json::array({{{"value", std::stoll(game_id)}, {"op", "EQUALS"}}})}};
    const std::string query = "query($f:ModFileContentSearchFilter,$o:Int,$c:Int){modFileContents(filter:$f,offset:$o,count:$c){totalCount nodes{filePath fileSize modId}}}";
    json files = json::array();
    std::string source = "graphql";
    int offset = 0, total = 0;
    do {
        json page = graphql(query, {{"f", filter}, {"o", offset}, {"c", 80}})["modFileContents"];
        total = page["totalCount"].get<int>();
        for (const auto& n : page["nodes"]) {
            if (id_number(n["modId"]) != t.mod_id()) fail("file " + file + " belongs to mod " + std::to_string(id_number(n["modId"])) + ", not " + t.mod + ".");
            std::string bytes = text_of(n, "fileSize");
            files.push_back({{"path", value_of(n, "filePath")}, {"size", bytes.empty() ? json(nullptr) : json(std::stoll(bytes))}});
        }
        offset += static_cast<int>(page["nodes"].size());
        if (page["nodes"].empty()) break;
    } while (offset < total && offset < 10000);

    if (files.empty()) {
        json info = v1_get(t.v1_mod() + "/files/" + file + ".json");
        std::string link = text_of(info, "content_preview_link");
        if (link.empty()) fail("Nexus Mods has no list of the files inside file " + file + ".");
        std::string text;
        Http r = api_send(Api::Preview, "GET", link, {"Accept: application/json"}, "", [&](const char* p, size_t n) { text.append(p, n); });
        if (r.status != 200) fail("Nexus Mods' content preview for file " + file + " isn't available (HTTP " + std::to_string(r.status) + ").");
        collect_preview(parse_reply(text, "content preview"), files);
        source = "content preview";
    }

    json plugins = json::array(), archives = json::array();
    for (const auto& f : files) {
        std::string p = lower(f["path"].is_string() ? f["path"].get<std::string>() : "");
        std::string ext = p.size() > 4 ? p.substr(p.size() - 4) : "";
        if (ext == ".esp" || ext == ".esm" || ext == ".esl") plugins.push_back(f["path"]);
        if (ext == ".bsa" || ext == ".ba2") archives.push_back(f["path"]);
    }
    json out = t.header();
    out["file_id"] = std::stoll(file);
    out["source"] = source;
    out["count"] = files.size();
    out["plugin_count"] = plugins.size();
    out["single_plugin"] = plugins.size() == 1;
    out["plugins"] = plugins;
    out["bsa_archives"] = archives;
    out["files"] = files;
    return out;
}

json ids_variable(const Target& t) {
    return {{"ids", json::array({{{"gameDomain", t.game}, {"modId", t.mod_id()}}})}};
}

json single_mod(const std::string& query, json variables) {
    json nodes = graphql(query, variables)["legacyModsByDomain"]["nodes"];
    if (nodes.empty()) {
        const json& id = variables["ids"][0];
        fail("Nexus Mods has no mod " + std::to_string(id.value("modId", 0LL)) + " for the game " + id.value("gameDomain", "") + ".");
    }
    return nodes[0];
}

long long id_number(const json& v) {
    if (v.is_number_integer()) return v.get<long long>();
    return v.is_string() ? std::strtoll(v.get<std::string>().c_str(), nullptr, 10) : 0;
}

// The modRequirements lists are paged. field is nexusRequirements or modsRequiringThisMod.
struct RequirementPage {
    std::vector<json> nodes;
    json mod;       // the extra_mod_fields of the first response
    int total = 0;  // every entry the mod has, not just the ones returned
};

RequirementPage requirement_nodes(const Target& t, const std::string& field, const std::string& node_fields,
                                  const std::string& extra_mod_fields) {
    const std::string query = "query($ids:[CompositeDomainWithIdInput!]!,$offset:Int,$count:Int){legacyModsByDomain(ids:$ids){nodes{" +
                              extra_mod_fields + " modRequirements{" + field + "(offset:$offset,count:$count){totalCount nodes{" +
                              node_fields + "}}}}}}";
    RequirementPage out;
    int offset = t.offset;
    bool first = true;
    while (static_cast<int>(out.nodes.size()) < t.limit) {
        json vars = ids_variable(t);
        vars["offset"] = offset;
        vars["count"] = std::min(kPageSize, t.limit - static_cast<int>(out.nodes.size()));
        json mod = single_mod(query, vars);
        if (first) out.mod = mod;
        first = false;
        const json& page = mod["modRequirements"][field];
        out.total = page["totalCount"].get<int>();
        for (const auto& n : page["nodes"]) out.nodes.push_back(n);
        offset += static_cast<int>(page["nodes"].size());
        if (page["nodes"].empty() || offset >= out.total) break;
    }
    return out;
}

void add_page_info(json& out, const Target& t, const RequirementPage& page) {
    int end = t.offset + static_cast<int>(page.nodes.size());
    out["total"] = page.total;
    out["offset"] = t.offset;
    out["returned"] = page.nodes.size();
    out["next_offset"] = !page.nodes.empty() && end < page.total ? json(end) : json(nullptr);
}

json cmd_reqs(const Target& t) {
    RequirementPage page = requirement_nodes(t, "nexusRequirements", "modId modName gameId url notes externalRequirement",
                                             "name legacyModRequirementsEnabled modRequirements{dlcRequirements{gameExpansion{name} notes}}");
    const json& mod = page.mod;
    json nexus = json::array(), external = json::array(), dlc = json::array();
    for (const auto& n : page.nodes) {
        if (n.value("externalRequirement", false)) {
            external.push_back({{"name", value_of(n, "modName")}, {"url", value_of(n, "url")}, {"notes", value_of(n, "notes")}});
        } else {
            nexus.push_back({{"mod_id", id_number(n["modId"])}, {"name", value_of(n, "modName")}, {"game_id", id_number(n["gameId"])},
                             {"notes", value_of(n, "notes")}});
        }
    }
    for (const auto& d : mod["modRequirements"]["dlcRequirements"])
        dlc.push_back({{"name", d["gameExpansion"].value("name", "")}, {"notes", value_of(d, "notes")}});
    json out = t.header();
    out["name"] = value_of(mod, "name");
    out["legacy_requirements"] = value_of(mod, "legacyModRequirementsEnabled");
    add_page_info(out, t, page);
    // One clear answer: does the mod list anything at all it needs (Nexus mods, external links or DLC)?
    out["has_requirements"] = page.total > 0 || !mod["modRequirements"]["dlcRequirements"].empty();
    out["nexus"] = nexus;
    out["external"] = external;
    out["dlc"] = dlc;
    return out;
}

json cmd_dependents(const Target& t) {
    RequirementPage page = requirement_nodes(t, "modsRequiringThisMod", "modId modName gameId notes", "name");
    json list = json::array();
    for (const auto& n : page.nodes)
        list.push_back({{"mod_id", id_number(n["modId"])}, {"name", value_of(n, "modName")}, {"game_id", id_number(n["gameId"])},
                        {"notes", value_of(n, "notes")}});
    json out = t.header();
    out["name"] = value_of(page.mod, "name");
    add_page_info(out, t, page);
    out["dependents"] = list;
    return out;
}

void add_mentions(json& mentions, const char* source, const std::string& text) {
    static const char* words[] = {"permission", "modif", "reupload", "re-upload", "allowed to", "feel free", "free to use", "patches"};
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = strip_bbcode(text.substr(start, end - start));
        std::string low = lower(line);
        for (const char* w : words) {
            if (low.find(w) != std::string::npos) {
                mentions.push_back({{"source", source}, {"text", line}});
                break;
            }
        }
        start = end + 1;
    }
}

std::string tag_text(const std::string& html) {
    std::string plain;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') {
            in_tag = true;
            plain += ' ';
        } else if (c == '>') {
            in_tag = false;
        } else if (!in_tag) {
            plain += c;
        }
    }
    std::string text = clean_text(plain), out;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The text after the next occurrence of `marker`, from its closing '>' up to `close`. Moves pos past it.
std::string element_after(const std::string& s, size_t& pos, const std::string& marker, const std::string& close) {
    size_t at = s.find(marker, pos);
    if (at == std::string::npos) return {};
    size_t open = s.find('>', at);
    if (open == std::string::npos) return {};
    size_t end = s.find(close, open);
    if (end == std::string::npos) return {};
    pos = end + close.size();
    return tag_text(s.substr(open + 1, end - open - 1));
}

// The "Permissions and credits" section of a mod page, or null if the page doesn't have one.
json parse_permissions(const std::string& page) {
    size_t heading = page.find("Permissions and credits");
    if (heading == std::string::npos) return nullptr;
    size_t dd = page.find("<dd", heading);
    size_t dd_end = dd == std::string::npos ? dd : page.find("</dd>", dd);
    if (dd_end == std::string::npos) return nullptr;
    std::string section = page.substr(dd, dd_end - dd);

    json sections = json::object();
    const std::string block_marker = "class=\"tabbed-block\"";
    for (size_t at = section.find(block_marker); at != std::string::npos;) {
        size_t next = section.find(block_marker, at + 1);
        std::string block = section.substr(at, next == std::string::npos ? next : next - at);
        size_t pos = 0;
        std::string title = element_after(block, pos, "<h3", "</h3>");
        size_t body = pos;
        json items = json::array();
        for (size_t li = block.find("<li", pos); li != std::string::npos; li = block.find("<li", pos)) {
            std::string head = block.substr(li, block.find('>', li) - li);
            pos = li + 3;
            const char* state = head.find("permission-yes") != std::string::npos     ? "yes"
                                : head.find("permission-no") != std::string::npos    ? "no"
                                : head.find("permission-maybe") != std::string::npos ? "maybe"
                                                                                     : nullptr;
            if (!state) continue;
            std::string item = element_after(block, pos, "permissions-title", "</span>");
            std::string text = element_after(block, pos, "permissions-desc", "</span>");
            items.push_back({{"state", state}, {"title", item}, {"text", text}});
        }
        if (!title.empty()) sections[title] = items.empty() ? json(tag_text(block.substr(body))) : items;
        at = next;
    }
    return sections;
}

// Browsers save a page as MHTML: a mail-style file with the HTML quoted-printable encoded. Plain HTML passes through.
std::string html_from_saved_page(const std::string& raw) {
    size_t multipart = raw.substr(0, 4096).find("multipart/related");
    if (multipart == std::string::npos) return raw;
    size_t b = raw.find("boundary=\"", multipart);
    size_t type = raw.find("Content-Type: text/html");
    size_t headers_end = type == std::string::npos ? type : raw.find("\r\n\r\n", type);
    if (b == std::string::npos || headers_end == std::string::npos) fail("couldn't find the page inside that saved file");
    b += 10;
    std::string boundary = "--" + raw.substr(b, raw.find('"', b) - b);
    size_t begin = headers_end + 4, end = raw.find(boundary, begin);
    std::string body = raw.substr(begin, end == std::string::npos ? end : end - begin);
    if (raw.substr(type, headers_end - type).find("quoted-printable") == std::string::npos) return body;
    std::string out;
    out.reserve(body.size());
    auto hex = [](char c) { return c <= '9' ? c - '0' : (c & ~0x20) - 'A' + 10; };
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '=') {
            out += body[i];
        } else if (i + 1 < body.size() && body[i + 1] == '\n') {
            i += 1;
        } else if (i + 2 < body.size() && body[i + 1] == '\r' && body[i + 2] == '\n') {
            i += 2;
        } else if (i + 2 < body.size() && std::isxdigit(static_cast<unsigned char>(body[i + 1])) && std::isxdigit(static_cast<unsigned char>(body[i + 2]))) {
            out += static_cast<char>(hex(body[i + 1]) * 16 + hex(body[i + 2]));
            i += 2;
        }
    }
    return out;
}

json cmd_permissions(const Target& t, const std::string& saved_page) {
    json out = t.header();
    out["page_url"] = t.page();
    std::string page;
    if (!saved_page.empty()) {
        page = html_from_saved_page(read_file(to_wide(saved_page)));
        out["source"] = "saved_page";
    } else {
        std::string text;
        Http r = send("GET", t.page(), {"Accept: text/html"}, "", [&](const char* p, size_t n) { text.append(p, n); });
        if (r.status == 200) {
            page = text;
            out["source"] = "live_page";
        } else {
            out["source"] = "api_only";
            out["page_status"] = r.status;
        }
    }
    if (!page.empty()) {
        json sections = parse_permissions(page);
        if (sections.is_null()) fail("that page has no Permissions and credits section. Unpublished and hidden pages only show it when you are signed in: save the page from your browser and pass it with --html <file>");
        out["sections"] = sections;
        return out;
    }

    // The page itself refused this program, so report what the API knows.
    json mod = single_mod("query($ids:[CompositeDomainWithIdInput!]!){legacyModsByDomain(ids:$ids){nodes{name author description uploader{name about}}}}",
                          ids_variable(t));
    json mentions = json::array();
    add_mentions(mentions, "description", clean_text(text_of(mod, "description")));
    if (mod.contains("uploader") && mod["uploader"].is_object())
        add_mentions(mentions, "uploader_profile", clean_text(text_of(mod["uploader"], "about")));
    out["name"] = value_of(mod, "name");
    out["author"] = value_of(mod, "author");
    out["uploader"] = mod["uploader"].is_object() ? value_of(mod["uploader"], "name") : json(nullptr);
    out["sections"] = nullptr;
    out["message"] = "Nexus didn't let this program read the page (see page_status). Open page_url in a browser, or save the page and pass it with --html <file>.";
    out["mentions"] = mentions;
    return out;
}

// ---------------------------------------------------------------- local files

std::string read_file(const fs::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) fail("cannot read " + path.u8string());
    std::string out;
    char buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
    bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) fail("cannot read " + path.u8string());
    return out;
}

// Reads the file back, so a backup that didn't really land is caught before anything is overwritten.
void write_file(const fs::path& path, const std::string& data) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) fail("cannot write " + path.u8string());
    size_t written = std::fwrite(data.data(), 1, data.size(), f);
    bool failed = std::fclose(f) != 0 || written != data.size();
    if (failed || read_file(path) != data) fail("cannot write " + path.u8string());
}

bool valid_utf8(const std::string& s) {
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

std::string utc_stamp() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}

// Removes a UTF-8 BOM, turns every line ending into \n and drops trailing whitespace.
std::string normalize_text(std::string s) {
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            out += '\n';
        } else {
            out += s[i];
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

// The API escapes < and & in what it is sent, so HTML tags and entities would show up on the page as literal text.
// Returns where the first one is, or "" if there are none.
std::string html_problem(const std::string& s) {
    static const std::set<std::string> tags = {"a", "b", "blockquote", "br", "center", "div", "em", "font", "h1", "h2", "h3", "h4",
                                               "h5", "h6", "hr", "i", "img", "li", "ol", "p", "pre", "span", "strong", "table", "td",
                                               "th", "tr", "u", "ul"};
    static const std::set<std::string> entities = {"amp", "lt", "gt", "quot", "apos", "nbsp"};
    int line = 1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') ++line;
        size_t end = 0;
        bool found = false;
        if (s[i] == '<') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '/') ++j;
            size_t name = j;
            while (j < s.size() && std::isalnum(static_cast<unsigned char>(s[j]))) ++j;
            found = j < s.size() && (s[j] == '>' || s[j] == ' ' || s[j] == '/') && tags.count(lower(s.substr(name, j - name))) > 0;
            end = s.find('>', j);
        } else if (s[i] == '&') {
            end = s.find(';', i);
            if (end != std::string::npos && end - i <= 10) {
                std::string name = lower(s.substr(i + 1, end - i - 1));
                found = entities.count(name) > 0 ||
                        (name.size() > 1 && name[0] == '#' && std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c); }));
            }
        }
        if (found) return "line " + std::to_string(line) + ": " + s.substr(i, end == std::string::npos ? 20 : std::min<size_t>(end - i + 1, 40));
    }
    return {};
}

void check_no_html(const std::string& what, const std::string& text, bool allow) {
    if (allow) return;
    std::string problem = html_problem(text);
    if (!problem.empty())
        fail(what + " has HTML in it (" + problem + "). Nexus would show that as literal text. Use BBCode and plain line breaks, " +
             "or pass --allow-html if it's meant to be shown literally");
}

// ---------------------------------------------------------------- changing a mod page

json v3_mod(const Target& t) { return v3_call("GET", "/games/" + t.game + "/mods/" + t.mod)["data"]; }

json cmd_save_desc(const Target& t, const std::string& dest, bool force) {
    json mod = v3_mod(t);
    fs::path target = to_wide(dest);
    if (fs::is_directory(target) || dest.back() == '/' || dest.back() == '\\') target /= to_wide(t.mod + "-description.bbcode");
    if (fs::exists(target) && !force) fail(target.u8string() + " already exists (--force replaces it)");
    std::string text = clean_text(text_of(mod, "description"));
    write_file(target, text);
    json out = t.header();
    out["name"] = value_of(mod, "name");
    out["path"] = fs::absolute(target).u8string();
    out["chars"] = text.size();
    return out;
}

// Two files: the BBCode, which set-desc can restore, and the text exactly as the server stored it.
json backup_description(const Target& t, const std::string& stored, const fs::path& folder) {
    if (stored.empty()) fail("could not read the current description, so it can't be backed up; nothing was changed");
    std::string base = t.mod + "-" + utc_stamp() + "-description";
    fs::path bbcode = folder / to_wide(base + ".bbcode"), raw = folder / to_wide(base + ".raw.txt");
    write_file(bbcode, clean_text(stored));
    write_file(raw, stored);
    return {{"bbcode", fs::absolute(bbcode).u8string()}, {"as_stored", fs::absolute(raw).u8string()}};
}

json cmd_set_desc(const Target& t, const std::string& file, std::string backup_dir, bool dry_run, bool allow_html) {
    fs::path input = to_wide(file);
    std::string raw_input = read_file(input);
    if (!valid_utf8(raw_input)) fail(file + " isn't valid UTF-8; save it as UTF-8 first");
    std::string text = normalize_text(raw_input);
    if (text.empty()) fail(file + " is empty");
    check_no_html(file, text, allow_html);

    json mod = v3_mod(t);
    std::string stored = text_of(mod, "description");
    fs::path folder = backup_dir.empty() ? fs::absolute(input).parent_path() / L"nexus-api-tool-backups" : fs::path(to_wide(backup_dir));
    json backup = backup_description(t, stored, folder);

    json out = t.header();
    out["name"] = value_of(mod, "name");
    out["backup"] = backup;
    out["chars"] = text.size();
    if (normalize_text(clean_text(stored)) == text) {
        out["changed"] = false;
        out["message"] = "the page already has this text; nothing was sent";
        return out;
    }
    if (dry_run) {
        out["changed"] = false;
        out["dry_run"] = true;
        out["message"] = "backed up and checked; nothing was sent";
        return out;
    }
    json body = {{"description", text}};
    v3_call("PATCH", "/mods/" + text_of(mod, "id"), &body);
    out["changed"] = true;

    std::string after = normalize_text(clean_text(text_of(v3_mod(t), "description")));
    out["verified"] = after == text;
    if (after != text) fail("the page was changed but reads back differently from " + file + ". The old text is in " + backup["bbcode"].get<std::string>());
    return out;
}

std::string md5_of(const std::string& data, bool hex) {
    unsigned char hash[16];
    if (BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, sizeof(hash)) != 0)
        fail("could not hash the file");
    if (hex) {
        std::string out;
        char piece[3];
        for (unsigned char b : hash) {
            std::snprintf(piece, sizeof(piece), "%02x", b);
            out += piece;
        }
        return out;
    }
    DWORD size = 0;
    CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size);
    std::string out(size, '\0');
    CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &size);
    out.resize(size);
    return out;
}

struct UploadArgs {
    std::string file, name, version, category, note, replaces, changelog;
    bool set_mod_version = false, dry_run = false, allow_html = false;
};

bool all_in(const std::string& s, const char* punctuation) {
    return std::all_of(s.begin(), s.end(), [&](unsigned char c) { return std::isalnum(c) || (c < 0x80 && std::strchr(punctuation, c)); });
}

json cmd_upload_file(const Target& t, const UploadArgs& a) {
    // Everything the API would refuse is checked first, so a bad name never costs an upload.
    if (a.name.empty() || a.name.size() > 50 || !all_in(a.name, " _'().-"))
        fail("--name must be 1 to 50 characters: letters, digits, spaces and _ ' ( ) . -");
    if (a.version.empty() || a.version.size() > 50 || !all_in(a.version, ".-"))
        fail("--file-version must be 1 to 50 characters: letters, digits and . -");
    if (!a.category.empty() && a.category != "main" && a.category != "optional" && a.category != "miscellaneous")
        fail("--category must be main, optional or miscellaneous");
    if (!valid_utf8(a.note) || !valid_utf8(a.changelog)) fail("--note and --changelog must be UTF-8");
    check_no_html("--note", a.note, a.allow_html);
    check_no_html("--changelog", a.changelog, a.allow_html);

    fs::path path = to_wide(a.file);
    if (!fs::is_regular_file(path)) fail(a.file + " isn't a file");
    long long size = static_cast<long long>(fs::file_size(path));
    if (size == 0) fail(a.file + " is empty");
    if (size >= kMaxUploadBytes) fail(a.file + " is 100 MiB or larger; this tool only does the API's simple upload, which is limited to that");
    std::string filename = path.filename().u8string();
    if (filename.find('"') != std::string::npos || std::any_of(filename.begin(), filename.end(), [](unsigned char c) { return c >= 0x80; }))
        fail("the file name has to be plain ASCII with no quotes: " + filename);

    json mod = v3_mod(t);
    std::string mod_uuid = text_of(mod, "id"), group, category = a.category;
    if (!a.replaces.empty()) {
        json version = v3_call("GET", "/games/" + t.game + "/mod-file-versions/" + a.replaces)["data"];
        group = text_of(version["file"], "id");
        if (group.empty()) fail("could not find the file group of file " + a.replaces);
        json groups = v3_call("GET", "/mods/" + mod_uuid + "/files")["data"]["mod_files"];
        if (std::none_of(groups.begin(), groups.end(), [&](const json& g) { return text_of(g, "id") == group; }))
            fail("file " + a.replaces + " doesn't belong to mod " + t.mod);
        if (category.empty()) {
            std::string old_category = text_of(version, "category");
            category = old_category == "optional" || old_category == "miscellaneous" ? old_category : "main";
        }
    }
    if (category.empty()) category = "main";

    json out = t.header();
    out["mod_name"] = value_of(mod, "name");
    out["file"] = filename;
    out["bytes"] = size;
    out["label"] = a.name;
    out["version"] = a.version;
    out["category"] = category;
    out["replaces"] = a.replaces.empty() ? json(nullptr) : json(std::stoll(a.replaces));
    if (a.dry_run) {
        out["dry_run"] = true;
        out["message"] = "checked; nothing was uploaded";
        return out;
    }

    std::string data = read_file(path);
    json request = {{"size_bytes", size}, {"filename", filename}, {"md5", md5_of(data, true)}};
    json upload = v3_call("POST", "/uploads", &request)["data"];
    std::string upload_id = text_of(upload, "id");
    // The presigned URL signs these three headers, so they must be exactly this.
    Http put = api_send(Api::Storage, "PUT", text_of(upload, "presigned_url"),
                    {"Content-Disposition: attachment; filename=\"" + filename + "\"", "Content-MD5: " + md5_of(data, false),
                     "Content-Type: application/octet-stream"},
                    data, [](const char*, size_t) {});
    if (put.status < 200 || put.status >= 300)
        fail("sending the file to Nexus Mods' storage failed: it answered with HTTP " + std::to_string(put.status) + ".");
    v3_call("POST", "/uploads/" + upload_id + "/finalise");
    std::string state;
    for (int tries = 0; tries < 90 && state != "available"; ++tries) {
        Sleep(2000);
        state = text_of(v3_call("GET", "/uploads/" + upload_id)["data"], "state");
    }
    if (state != "available") fail("Nexus never finished processing the upload (state: " + state + ")");

    json file = {{"upload_id", upload_id}, {"name", a.name}, {"version", a.version}, {"file_category", category},
                 {"show_requirements_pop_up", false}, {"allow_mod_manager_download", true}, {"update_mod_version", a.set_mod_version}};
    if (!a.note.empty()) file["description"] = a.note;
    json created;
    if (a.replaces.empty()) {
        file["mod_id"] = mod_uuid;
        created = v3_call("POST", "/mod-files", &file);
    } else {
        file["archive_existing_file"] = true;
        created = v3_call("POST", "/mod-files/" + group + "/versions", &file);
    }
    out["uploaded"] = true;
    out["result"] = created.is_object() ? value_of(created, "data") : json(nullptr);
    if (!a.changelog.empty()) {
        json entry = {{"version", a.version}, {"changelog", a.changelog}};
        v3_call("POST", "/mods/" + mod_uuid + "/changelogs", &entry);
        out["changelog_added"] = true;
    }
    return out;
}

// ---------------------------------------------------------------- list-mods

json cmd_list_mods(const Target& t, const std::string& user) {
    json found = graphql("query($n:String!){userByName(name:$n){memberId name}}", {{"n", user}})["userByName"];
    if (!found.is_object()) fail("no Nexus user is called " + user);
    json filter = {{"uploaderId", json::array({{{"value", std::to_string(found["memberId"].get<long long>())}, {"op", "EQUALS"}}})},
                   {"gameDomainName", json::array({{{"value", t.game}, {"op", "EQUALS"}}})}};
    const std::string query =
        "query($f:ModsFilter,$o:Int,$c:Int){mods(filter:$f,offset:$o,count:$c,sort:[{createdAt:{direction:DESC}}]){totalCount "
        "nodes{modId name version status adultContent createdAt updatedAt downloads endorsements author}}}";
    json list = json::array();
    int offset = t.offset, total = 0;
    while (static_cast<int>(list.size()) < t.limit) {
        json vars = {{"f", filter}, {"o", offset}, {"c", std::min(kPageSize, t.limit - static_cast<int>(list.size()))}};
        json page = graphql(query, vars)["mods"];
        total = page["totalCount"].get<int>();
        for (const auto& n : page["nodes"])
            list.push_back({{"mod_id", id_number(n["modId"])}, {"name", value_of(n, "name")}, {"version", value_of(n, "version")},
                            {"author", value_of(n, "author")}, {"status", value_of(n, "status")}, {"adult", value_of(n, "adultContent")},
                            {"created", value_of(n, "createdAt")}, {"updated", value_of(n, "updatedAt")},
                            {"downloads", value_of(n, "downloads")}, {"endorsements", value_of(n, "endorsements")}});
        offset += static_cast<int>(page["nodes"].size());
        if (page["nodes"].empty() || offset >= total) break;
    }
    int end = t.offset + static_cast<int>(list.size());
    json out = {{"user", value_of(found, "name")}, {"member_id", value_of(found, "memberId")}, {"game", t.game},
                {"total", total}, {"offset", t.offset}, {"returned", list.size()},
                {"next_offset", !list.empty() && end < total ? json(end) : json(nullptr)}, {"mods", list}};
    return out;
}

// ---------------------------------------------------------------- search-files (this copy only)

// Every archive file whose contents match, from GraphQL's file-contents index. The index only serves the first
// 10,000 results of one query, so a mod-id range with more is split in halves until each part fits; pages are 80.
json cmd_search_files(const std::string& game, const std::string& ext, long long lo, long long hi, const std::string& name, long long max_results) {
    long long game_id = std::stoll(game_number(game));
    auto filter_for = [&](long long a, long long b) {
        json f = {{"gameId", json::array({{{"value", game_id}, {"op", "EQUALS"}}})},
                  {"modId", json::array({{{"value", a}, {"op", "GTE"}}, {{"value", b}, {"op", "LTE"}}})}};
        if (!ext.empty()) f["fileExtensionExact"] = json::array({{{"value", ext}, {"op", "EQUALS"}}});
        if (!name.empty()) f["fileNameWildcard"] = json::array({{{"value", name}, {"op", "WILDCARD"}}});
        return f;
    };
    const std::string query = "query($f:ModFileContentSearchFilter,$o:Int,$c:Int){modFileContents(filter:$f,offset:$o,count:$c){totalCount nodes{modId fileId filePath fileSize}}}";
    auto total_of = [&](long long a, long long b) { return graphql(query, {{"f", filter_for(a, b)}, {"o", 0}, {"c", 1}})["modFileContents"]["totalCount"].get<long long>(); };

    std::vector<std::pair<long long, long long>> todo = {{lo, hi}}, parts;
    long long total = 0;
    while (!todo.empty()) {
        auto [a, b] = todo.back();
        todo.pop_back();
        long long n = total_of(a, b);
        if (n == 0) continue;
        if (n > 10000 && b > a) {
            long long mid = a + (b - a) / 2;
            todo.push_back({mid + 1, b});
            todo.push_back({a, mid});
            continue;
        }
        if (n > 10000) fail("mod " + std::to_string(a) + " alone has more than 10,000 matching files; narrow the search with --name.");
        parts.push_back({a, b});
        total += n;
    }
    std::sort(parts.begin(), parts.end());

    json rows = json::array();
    bool truncated = false;
    for (const auto& [a, b] : parts) {
        int offset = 0;
        for (;;) {
            if (static_cast<long long>(rows.size()) >= max_results) { truncated = true; break; }
            json page = graphql(query, {{"f", filter_for(a, b)}, {"o", offset}, {"c", 80}})["modFileContents"];
            for (const auto& n : page["nodes"]) {
                if (static_cast<long long>(rows.size()) >= max_results) { truncated = true; break; }
                std::string bytes = text_of(n, "fileSize");
                rows.push_back({{"mod_id", id_number(n["modId"])}, {"file_id", id_number(n["fileId"])}, {"path", value_of(n, "filePath")},
                                {"size", bytes.empty() ? json(nullptr) : json(std::stoll(bytes))}});
            }
            offset += static_cast<int>(page["nodes"].size());
            if (truncated || page["nodes"].empty() || offset >= page["totalCount"].get<int>()) break;
        }
        if (truncated) break;
    }
    return {{"game", game}, {"extension", ext.empty() ? json(nullptr) : json(ext)}, {"name", name.empty() ? json(nullptr) : json(name)},
            {"mod_range", json::array({lo, hi})}, {"total", total}, {"returned", rows.size()}, {"truncated", truncated},
            {"note", truncated ? json("stopped at --max-results; narrow --mod-range or raise --max-results") : json(nullptr)}, {"files", rows}};
}

// ---------------------------------------------------------------- usage report and --help --verbose

struct CallCost {
    const char* command;
    int v1, graphql, v3, downloads, uploads;  // uploads are counted in v3 too
    const char* note;
};

const std::vector<CallCost>& call_costs() {
    static const std::vector<CallCost> costs = {
        {"desc", 1, 0, 0, 0, 0, ""},
        {"changelog", 1, 0, 0, 0, 0, ""},
        {"files", 0, 1, 0, 0, 0, "+1 GraphQL for a game other than Skyrim Special Edition"},
        {"archived-files", 0, 1, 0, 0, 0, "+1 GraphQL for a game other than Skyrim Special Edition"},
        {"file-note", 0, 1, 0, 0, 0, "+1 GraphQL for a game other than Skyrim Special Edition"},
        {"contents", 0, 2, 0, 0, 0, "+1 GraphQL per 80 files; +1 v1 and a preview if GraphQL hasn't indexed the file"},
        {"dl-file", 2, 0, 0, 1, 0, "1 v1 and no download when --skip-existing or --max-size skips the file"},
        {"reqs", 0, 1, 0, 0, 0, "+1 GraphQL per 50 entries"},
        {"dependents", 0, 1, 0, 0, 0, "+1 GraphQL per 50 entries"},
        {"list-mods", 0, 2, 0, 0, 0, "+1 GraphQL per 50 mods"},
        {"search-files", 0, 2, 0, 0, 0, "+1 GraphQL per 80 results, +1 per range split over 10,000 results"},
        {"save-desc", 0, 0, 1, 0, 0, ""},
        {"set-desc", 0, 0, 3, 0, 0, "1 with --dry-run"},
        {"upload-file", 0, 0, 6, 0, 1, "about 6 v3 calls; more while Nexus processes a big file; Cloudflare limits the upload"},
    };
    return costs;
}

std::string rate_text(double runs) {
    char buffer[32];
    if (runs < 10) std::snprintf(buffer, sizeof(buffer), "%.1f", runs);
    else return with_commas(static_cast<long long>(runs));
    return buffer;
}

std::string help_verbose() {
    std::string out = "\nCALLS PER RUN, AND THE MOST RUNS THE PACING AND THE API LIMITS ALLOW\n";
    out += "  pacing: queries 1 per " + rate_text(gap_ms(Kind::Query) / 1000.0) + " s, downloads 1 per " + rate_text(gap_ms(Kind::Download) / 1000.0) +
           " s, uploads 1 per " + rate_text(gap_ms(Kind::Upload) / 1000.0) + " s\n";
    char line[256];
    std::snprintf(line, sizeof(line), "  %-15s %3s %8s %3s %9s %7s   %8s %9s %9s\n", "command", "v1", "GraphQL", "v3", "downloads", "uploads",
                  "per min", "per hour", "per day");
    out += line;
    for (const auto& c : call_costs()) {
        long long queries = c.v1 + c.graphql + c.v3 - c.uploads;
        long long run_ms = std::max({queries * gap_ms(Kind::Query), c.downloads * gap_ms(Kind::Download), c.uploads * gap_ms(Kind::Upload), 1LL});
        double per_min = 60000.0 / run_ms, per_hour = 3600000.0 / run_ms, per_day = 86400000.0 / run_ms;
        for (int n : {c.v1, c.v3}) {
            if (n > 0) {
                per_min = std::min(per_min, static_cast<double>(kHourlyLimit) / n);
                per_hour = std::min(per_hour, static_cast<double>(kHourlyLimit) / n);
                per_day = std::min(per_day, static_cast<double>(kDailyLimit) / n);
            }
        }
        std::snprintf(line, sizeof(line), "  %-15s %3d %8d %3d %9d %7d   %8s %9s %9s\n", c.command, c.v1, c.graphql, c.v3, c.downloads, c.uploads,
                      rate_text(per_min).c_str(), rate_text(per_hour).c_str(), rate_text(per_day).c_str());
        out += line;
        if (*c.note) out += std::string("                  ") + c.note + "\n";
    }
    out += "  v1 and v3 each allow 2,000 requests an hour and 20,000 a day; GraphQL reports no limit.\n";
    out += "  upload-file: Cloudflare's upload limit is inconsistent with their stated instructions e.g. \"retry_after: 30\".\n"
           "  Once this limit is tripped Cloudflare may impose a limit of 4/hour.\n";
    return out;
}

bool parse_per(const std::string& unit, long long& ms, std::string& label) {
    std::string u = lower(unit);
    if (u == "s" || u == "sec" || u == "second" || u == "seconds") { ms = 1000; label = "second"; }
    else if (u == "m" || u == "min" || u == "minute" || u == "minutes") { ms = 60000; label = "minute"; }
    else if (u == "h" || u == "hr" || u == "hour" || u == "hours") { ms = 3600000; label = "hour"; }
    else if (u == "d" || u == "day" || u == "days") { ms = 86400000; label = "day"; }
    else return false;
    return true;
}

json cmd_usage(bool verbose, long long window_ms, const std::string& label) {
    long long now = now_ms(), since = now - window_ms, hour_ago = now - 3600000;
    std::map<std::string, long long> by_api, hour_by_api;
    std::map<std::string, std::map<std::string, long long>> by_command;
    long long requests = 0, downloads = 0, uploads = 0, nexus_limited = 0, cloudflare_limited = 0;
    std::error_code ec;
    if (fs::exists(usage_path(), ec)) {
        std::string text = read_file(usage_path());
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            json e = json::parse(text.substr(start, end - start), nullptr, false);
            start = end + 1;
            if (!e.is_object()) continue;
            long long t = e.value("t", 0LL);
            std::string api = e.value("api", ""), kind = e.value("kind", "");
            if (t >= hour_ago) ++hour_by_api[api];
            if (t < since) continue;
            if (api == "upload storage") continue;  // part of the upload just counted
            ++requests;
            ++by_api[api];
            if (kind == "download") ++downloads;
            if (kind == "upload") ++uploads;
            if (e.value("rate_limited_by", "") == "Cloudflare") ++cloudflare_limited;
            if (e.value("rate_limited_by", "") == "Nexus Mods") ++nexus_limited;
            std::string cmd = e.value("command", "");
            ++by_command[cmd.empty() ? "(unknown)" : cmd][api];
        }
    }

    json state = load_state();
    json remaining = json::object();
    std::vector<std::string> summary;
    summary.push_back("In the last " + label + ": " + with_commas(requests) + " requests (" + with_commas(by_api["v1"]) + " v1, " +
                      with_commas(by_api["graphql"]) + " GraphQL, " + with_commas(by_api["v3"]) + " v3), " + with_commas(downloads) +
                      " downloads and " + with_commas(uploads) + " uploads.");
    for (const char* api : {"v1", "v3"}) {
        json r;
        if (state.contains("limits") && state["limits"].contains(api)) {
            const json& l = state["limits"][api];
            bool hour_reset = l.value("hourly_reset", 0LL) <= now, day_reset = l.value("daily_reset", 0LL) <= now;
            long long hourly = hour_reset ? l.value("hourly_limit", kHourlyLimit) : l.value("hourly_remaining", 0LL);
            long long daily = day_reset ? l.value("daily_limit", kDailyLimit) : l.value("daily_remaining", 0LL);
            r = {{"this_hour", hourly}, {"hourly_limit", l.value("hourly_limit", kHourlyLimit)},
                 {"hour_resets", hour_reset ? json(nullptr) : json(iso_ms(l.value("hourly_reset", 0LL)))},
                 {"today", daily}, {"daily_limit", l.value("daily_limit", kDailyLimit)},
                 {"day_resets", day_reset ? json(nullptr) : json(iso_ms(l.value("daily_reset", 0LL)))},
                 {"as_of", iso_ms(l.value("as_of", 0LL))}, {"made_by_this_tool_last_hour", hour_by_api[api]}};
            summary.push_back(std::string(api) + " API: " + with_commas(hourly) + " of " + with_commas(l.value("hourly_limit", kHourlyLimit)) +
                              " requests left this hour" + (hour_reset ? " (the hour has reset since the last request)" : ", resets at " + clock_utc(l.value("hourly_reset", 0LL))) +
                              "; " + with_commas(daily) + " of " + with_commas(l.value("daily_limit", kDailyLimit)) + " left today. As Nexus Mods reported at " +
                              iso_ms(l.value("as_of", 0LL)) + ".");
        } else {
            r = {{"this_hour", nullptr}, {"note", "no request to this API recorded yet, so Nexus Mods hasn't reported the limits"}};
            summary.push_back(std::string(api) + " API: no request recorded yet, so the remaining count isn't known. The limits are 2,000 an hour and 20,000 a day.");
        }
        remaining[api] = r;
    }
    remaining["graphql"] = "Nexus Mods publishes no limit for GraphQL and reports none.";
    if (nexus_limited > 0) summary.push_back("Nexus Mods refused " + with_commas(nexus_limited) + " requests in this window for the API limit.");
    if (cloudflare_limited > 0) summary.push_back("Cloudflare refused " + with_commas(cloudflare_limited) + " requests in this window.");

    json upload_section = {{"in_window", uploads}, {"note", "Upload limits are handled by Cloudflare."},
                           {"last_cloudflare_limit", state.contains("cloudflare_limited") ? json(iso_ms(state["cloudflare_limited"].value("at", 0LL))) : json(nullptr)}};
    json out = {{"window", "last " + label}, {"since", iso_ms(since)}, {"usage_file", usage_path().u8string()},
                {"requests", requests}, {"by_api", by_api}, {"downloads", downloads}, {"remaining", remaining}, {"uploads", upload_section},
                {"summary", summary}};
    if (verbose) out["by_command"] = by_command;
    return out;
}

// ---------------------------------------------------------------- command line

bool all_digits(const std::string& s) {
    return !s.empty() && s.size() <= 12 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool valid_game(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

void print(const std::string& text) { std::fwrite(text.data(), 1, text.size(), stdout); }

struct CommandSpec {
    const char* name;
    int positional;                 // arguments after the command
    std::set<std::string> options;  // besides --game, --interval and --list
    bool listable;                  // may run from --list
};

const std::vector<CommandSpec>& command_specs() {
    static const std::set<std::string> paged = {"--limit", "--offset"};
    static const std::vector<CommandSpec> specs = {
        {"search-files", 0, {"--ext", "--mod-range", "--name", "--max-results"}, false},
        {"desc", 1, {"--plain"}, true},
        {"changelog", 1, {"--plain"}, true},
        {"files", 1, {}, true},
        {"archived-files", 1, {}, true},
        {"dl-file", 2, {"--dest", "--force", "--skip-existing", "--max-size"}, true},
        {"file-note", 2, {"--plain"}, true},
        {"contents", 2, {}, true},
        {"permissions", 1, {"--html"}, false},
        {"reqs", 1, paged, true},
        {"dependents", 1, paged, true},
        {"list-mods", 1, paged, true},
        {"save-desc", 1, {"--dest", "--force"}, true},
        {"set-desc", 1, {"--file", "--backup-dir", "--dry-run", "--allow-html"}, false},
        {"upload-file", 1, {"--file", "--name", "--file-version", "--replaces", "--category", "--note", "--changelog", "--set-mod-version", "--dry-run", "--allow-html"}, false},
    };
    return specs;
}

std::string env_or_registry(const wchar_t* name) {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(name, buf, 512);
    if (n > 0 && n < 512) return to_utf8(buf);
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return to_utf8(buf);
    return {};
}

// A game's domain, from a domain or a Nexus game number.
std::string resolve_game(const std::string& game, const char* where) {
    if (all_digits(game)) {
        if (game == "1704") return kDefaultGame;
        json g = graphql("query($i:ID!){game(id:$i){domainName}}", {{"i", game}})["game"];
        if (!g.is_object() || !g["domainName"].is_string()) fail(std::string("Nexus Mods has no game number ") + game + " (from " + where + ").");
        return g["domainName"].get<std::string>();
    }
    if (!valid_game(game)) bad_args(std::string("\"") + game + "\" (from " + where + ") isn't a game domain or number");
    return game;
}

long long parse_size(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == 0 || i > 15) bad_args("--max-size must be a number of bytes, or end in KB, MB or GB");
    long long n = std::stoll(s.substr(0, i));
    std::string unit = lower(s.substr(i));
    if (unit.empty() || unit == "b") return n;
    if (unit == "k" || unit == "kb") return n * 1024;
    if (unit == "m" || unit == "mb") return n * 1024 * 1024;
    if (unit == "g" || unit == "gb") return n * 1024 * 1024 * 1024;
    bad_args("--max-size must be a number of bytes, or end in KB, MB or GB");
}

// One run of one command. positional[0] is the command.
json run_one(const CommandSpec& spec, const std::vector<std::string>& positional, std::map<std::string, std::string>& options, const std::string& game) {
    auto has = [&](const char* k) { return options.count(k) > 0; };
    auto option = [&](const char* k) { return has(k) ? options[k] : std::string(); };
    const std::string& command = positional[0];
    g_run.command = command;
    if (command == "search-files") {
        if (positional.size() != 1) bad_args("search-files takes no mod id; use --mod-range A-B");
        long long lo = 1, hi = 999999;
        std::string range = option("--mod-range");
        if (!range.empty()) {
            size_t dash = range.find('-');
            if (dash == std::string::npos || !all_digits(range.substr(0, dash)) || !all_digits(range.substr(dash + 1)))
                bad_args("--mod-range must look like 1000-2000");
            lo = std::stoll(range.substr(0, dash));
            hi = std::stoll(range.substr(dash + 1));
            if (lo > hi) bad_args("--mod-range must go from the lower id to the higher");
        }
        std::string ext = option("--ext");
        if (!ext.empty() && ext[0] != '.') ext = "." + ext;
        long long max_results = 10000;
        if (has("--max-results")) {
            if (!all_digits(option("--max-results"))) bad_args("--max-results must be a number");
            max_results = std::stoll(option("--max-results"));
            if (max_results < 1 || max_results > 200000) bad_args("--max-results must be 1 to 200,000");
        }
        if (ext.empty() && !has("--name")) bad_args("search-files needs --ext or --name");
        return cmd_search_files(game, lower(ext), lo, hi, option("--name"), max_results);
    }
    bool lists_user = command == "list-mods";
    bool takes_file = spec.positional == 2;
    if (static_cast<int>(positional.size()) != spec.positional + 1)
        bad_args(command + (lists_user ? " needs a user name" : takes_file ? " needs a mod id and a file number" : " needs a mod id"));
    if (!lists_user && !all_digits(positional[1])) bad_args("the mod id must be a number");
    if (takes_file && !all_digits(positional[2])) bad_args("the file number must be a number");
    if ((command == "dl-file" || command == "save-desc") && option("--dest").empty()) bad_args(command + " needs --dest <path>");
    if ((command == "set-desc" || command == "upload-file") && option("--file").empty()) bad_args(command + " needs --file <path>");
    if (command == "upload-file" && (option("--name").empty() || option("--file-version").empty())) bad_args("upload-file needs --name and --file-version");
    if (has("--replaces") && !all_digits(option("--replaces"))) bad_args("--replaces must be a file number");

    Target t{game, lists_user ? std::string("0") : positional[1]};
    std::string limit_text = option("--limit"), offset_text = option("--offset");
    if (!limit_text.empty() && (!all_digits(limit_text) || limit_text.size() > 9)) bad_args("--limit must be a number");
    if (!offset_text.empty() && (!all_digits(offset_text) || offset_text.size() > 9)) bad_args("--offset must be a number");
    if (!offset_text.empty()) t.offset = std::stoi(offset_text);
    if (!limit_text.empty()) t.limit = std::stoi(limit_text);
    if (t.limit < 1 || t.limit > kMaxLimit) bad_args("--limit must be 1 to " + std::to_string(kMaxLimit) + "; use --offset to read further");

    if (command == "desc") return cmd_desc(t, has("--plain"));
    if (command == "changelog") return cmd_changelog(t, has("--plain"));
    if (command == "files") return cmd_files(t, false);
    if (command == "archived-files") return cmd_files(t, true);
    if (command == "dl-file") {
        DownloadOptions o;
        o.dest = option("--dest");
        o.force = has("--force");
        o.skip_existing = has("--skip-existing");
        if (has("--max-size")) o.max_size = parse_size(option("--max-size"));
        return cmd_dl_file(t, positional[2], o);
    }
    if (command == "file-note") return cmd_file_note(t, positional[2], has("--plain"));
    if (command == "contents") return cmd_contents(t, positional[2]);
    if (command == "permissions") return cmd_permissions(t, option("--html"));
    if (command == "reqs") return cmd_reqs(t);
    if (command == "dependents") return cmd_dependents(t);
    if (command == "list-mods") return cmd_list_mods(t, positional[1]);
    if (command == "save-desc") return cmd_save_desc(t, option("--dest"), has("--force"));
    if (command == "set-desc") return cmd_set_desc(t, option("--file"), option("--backup-dir"), has("--dry-run"), has("--allow-html"));
    UploadArgs a;
    a.file = option("--file");
    a.name = option("--name");
    a.version = option("--file-version");
    a.category = option("--category");
    a.note = option("--note");
    a.replaces = option("--replaces");
    a.changelog = option("--changelog");
    a.set_mod_version = has("--set-mod-version");
    a.dry_run = has("--dry-run");
    a.allow_html = has("--allow-html");
    return cmd_upload_file(t, a);
}

// --list: one run per line, one JSON object per line on stdout.
int run_list(const CommandSpec& spec, const std::string& list_file, std::map<std::string, std::string>& options, const std::string& game) {
    std::string text = normalize_text(read_file(to_wide(list_file)));
    std::vector<std::pair<int, std::vector<std::string>>> items;
    int line_no = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        ++line_no;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::vector<std::string> args = {spec.name};
        std::string token;
        for (char c : line + " ") {
            if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == '|') {
                if (!token.empty()) args.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        if (args.size() > 1) items.push_back({line_no, args});
        if (end == text.size()) break;
    }
    if (items.empty()) bad_args(list_file + " has no items");

    int failed = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        json row = {{"line", items[i].first}, {"args", json(std::vector<std::string>(items[i].second.begin() + 1, items[i].second.end()))}};
        try {
            row["ok"] = true;
            row["result"] = run_one(spec, items[i].second, options, game);
        } catch (const RateLimited& e) {
            row["ok"] = false;
            row["error"] = e.what();
            row["rate_limited_by"] = e.by;
            row["retry_after"] = e.retry_after;
            print(row.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
            std::fprintf(stderr, "Stopped at line %d (item %zu of %zu): %s\n", items[i].first, i + 1, items.size(), e.what());
            return 3;
        } catch (const Failure& e) {
            if (e.code == 2) throw;  // a bad argument applies to every line
            row["ok"] = false;
            row["error"] = e.what();
            ++failed;
        } catch (const std::exception& e) {
            row["ok"] = false;
            row["error"] = e.what();
            ++failed;
        }
        print(row.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
        std::fflush(stdout);
    }
    if (failed > 0) std::fprintf(stderr, "%d of %zu items failed; see \"error\" on their lines.\n", failed, items.size());
    return failed > 0 ? 1 : 0;
}

int run(const std::vector<std::string>& args) {
    static const std::set<std::string> with_value = {"--game", "--dest", "--offset", "--file", "--backup-dir", "--name", "--file-version", "--replaces",
                                                     "--category", "--note", "--changelog", "--list", "--interval", "--max-size", "--per", "--html", "--ext", "--mod-range", "--max-results"};
    static const std::set<std::string> flags = {"--force", "--dry-run", "--allow-html", "--set-mod-version", "--skip-existing", "--verbose",
                                                "--rate", "--rates", "--limits", "--usage", "--plain"};
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    bool help = false, limit_flag = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h" || a == "help") {
            help = true;
        } else if (a == "--version") {
            print(std::string(kToolName) + " " + kToolVersion + "\n");
            return 0;
        } else if (a == "--limit") {  // a page size after a command, the usage report on its own
            limit_flag = true;
            if (i + 1 < args.size() && all_digits(args[i + 1])) options[a] = args[++i];
        } else if (with_value.count(a)) {
            if (i + 1 >= args.size()) bad_args(a + " needs a value");
            options[a] = args[++i];
        } else if (flags.count(a)) {
            options[a] = "1";
        } else if (a.rfind("--", 0) == 0) {
            bad_args("unknown option " + a);
        } else {
            positional.push_back(a);
        }
    }
    auto has = [&](const char* k) { return options.count(k) > 0; };
    if (has("--interval")) {
        char* end = nullptr;
        double s = std::strtod(options["--interval"].c_str(), &end);
        if (end == options["--interval"].c_str() || *end != '\0' || s < 0 || s > 3600) bad_args("--interval must be a number of seconds from 0 to 3600");
        g_run.gap_ms = static_cast<long long>(s * 1000 + 0.5);
    }
    if (help) {
        print(kUsage);
        if (has("--verbose")) print(help_verbose());
        return 0;
    }

    bool report = has("--rate") || has("--rates") || has("--limits") || has("--usage") || (limit_flag && positional.empty());
    if (report) {
        if (!positional.empty()) bad_args("--usage (--rate, --rates, --limits) runs on its own, without a command");
        long long window = 3600000;
        std::string label = "hour";
        if (has("--per") && !parse_per(options["--per"], window, label)) bad_args("--per takes s, sec, second, m, min, minute, h, hr, hour, d or day");
        print(cmd_usage(has("--verbose"), window, label).dump(2, ' ', false, json::error_handler_t::replace) + "\n");
        return 0;
    }
    if (positional.empty()) {
        print(kUsage);
        return 2;
    }

    const std::string& command = positional[0];
    const auto& specs = command_specs();
    auto spec = std::find_if(specs.begin(), specs.end(), [&](const CommandSpec& c) { return command == c.name; });
    if (spec == specs.end()) bad_args("unknown command " + command);
    for (const auto& kv : options) {
        if (kv.first == "--game" || kv.first == "--interval" || kv.first == "--list") continue;
        if (!spec->options.count(kv.first)) {
            if (kv.first == "--limit" || kv.first == "--offset") bad_args("--limit and --offset only apply to reqs, dependents and list-mods");
            if (kv.first == "--verbose" || kv.first == "--per") bad_args(kv.first + " goes with --help or --usage");
            bad_args(kv.first + " doesn't apply to " + command);
        }
    }

    // permissions also takes a mod page's address
    if (command == "permissions" && positional.size() == 2 && !all_digits(positional[1])) {
        std::string url = positional[1];
        size_t mods = url.find("/mods/");
        size_t host = url.find("nexusmods.com/");
        if (host == std::string::npos || mods == std::string::npos) bad_args("the mod id must be a number or a mod page's address");
        size_t id_end = url.find_first_not_of("0123456789", mods + 6);
        std::string id = url.substr(mods + 6, id_end == std::string::npos ? std::string::npos : id_end - mods - 6);
        size_t from = host + 14;
        std::string domain = url.substr(from, mods - from);
        if (domain.rfind("games/", 0) == 0) domain.erase(0, 6);
        if (!all_digits(id) || !valid_game(domain)) bad_args("that isn't a mod page's address");
        positional[1] = id;
        options["--game"] = domain;
    }
    std::string game_text = has("--game") ? options["--game"] : env_or_registry(L"NEXUS_GAME_ID");
    const char* where = has("--game") ? "--game" : "NEXUS_GAME_ID";
    std::string game = game_text.empty() ? std::string(kDefaultGame) : resolve_game(game_text, where);

    if (has("--list")) {
        if (!spec->listable) bad_args(command + " changes a mod page, so it doesn't run from --list");
        if (positional.size() > 1) bad_args("with --list, the items come from the file; give only the command");
        return run_list(*spec, options["--list"], options, game);
    }
    json result = run_one(*spec, positional, options, game);
    print(result.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(to_utf8(argv[i]));
    try {
        return run(args);
    } catch (const RateLimited& e) {
        json j = {{"error", e.what()}, {"rate_limited_by", e.by}, {"retry_after", e.retry_after}};
        print(j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
        std::fprintf(stderr, "error: %s\n", e.what());
        return e.code;
    } catch (const Failure& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return e.code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
