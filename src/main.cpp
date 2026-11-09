// nexus-api-tool: read a Nexus Mods page's information from the command line. See README.md.
//
// Sources: the v1 REST API (needs an API key) for description, changelog and files, the public v2 GraphQL API for
// requirements and dependents. Everything is printed as JSON (UTF-8) on stdout; errors go to stderr.

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

constexpr const char* kUsage = R"(nexus-api-tool - read a Nexus Mods page's information as JSON

USAGE
  nexus-api-tool <command> <mod id> [arguments] [--game <domain>]

COMMANDS
  desc <mod>                          name, author, version, summary and description
  changelog <mod>                     the changelog, per version
  files <mod>                         every file that isn't archived, grouped by category
                                      (main, update, optional, miscellaneous, old)
  archived-files <mod>                the same for archived and removed files
  dl-file <mod> <file#> --dest <path> download one file; <path> is a folder or a file name.
                                      Needs a Premium account. --force replaces an existing file
  file-note <mod> <file#>             the note on one file
  permissions <mod or address>        the page's Permissions and credits section. If Nexus turns the program
                                      away, --html <saved page> reads a page saved from a browser (see README)
  reqs <mod>                          what this mod requires
  dependents <mod>                    the mods that require this mod
  list-mods <user>                    the mods a user uploaded, newest first
                                      reqs, dependents and list-mods return 50 entries by default,
                                      at most 200 per call; use next_offset from the output with --offset

  save-desc <mod> --dest <path>       save the description page as a BBCode text file
  set-desc <mod> --file <path>        REPLACE the description page with a BBCode text file. The current
                                      page is backed up first; if that fails nothing is changed.
                                      Options: --backup-dir <folder>, --dry-run, --allow-html
  upload-file <mod> --file <path> --name <label> --file-version <v>
                                      upload an archive as a new file (under 100 MiB).
                                      Options: --replaces <file#> (new version of that file, the old one
                                      is archived), --category main|optional|miscellaneous, --note <text>,
                                      --changelog <text>, --set-mod-version, --dry-run, --allow-html

OPTIONS
  --game <domain>   the game's Nexus domain (default: skyrimspecialedition)
  --limit <n>       reqs, dependents, list-mods: entries to return (default 50, at most 200)
  --offset <n>      reqs, dependents, list-mods: entries to skip (default 0)
  --help, --version

API KEY
  Everything except permissions, reqs, dependents and list-mods reads your personal API key from the
  NEXUS_API_KEY environment variable (nexusmods.com > Site preferences > API Keys).

EXIT CODES
  0 ok   1 the request failed   2 bad arguments
)";

struct Failure : std::runtime_error {
    int code;
    Failure(int exit_code, const std::string& message) : std::runtime_error(message), code(exit_code) {}
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
    std::string error_body;  // the response body when status isn't 200
};

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
        fail("the request failed (WinHTTP error " + std::to_string(GetLastError()) + ")");

    Http result;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);

    std::vector<char> buffer(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got))
            fail("the connection dropped (WinHTTP error " + std::to_string(GetLastError()) + ")");
        if (got == 0) break;
        if (result.status >= 200 && result.status < 300)
            sink(buffer.data(), got);
        else if (result.error_body.size() < 4096)
            result.error_body.append(buffer.data(), got);
    }
    return result;
}

std::string api_error(int status, const std::string& body) {
    std::string detail;
    json j = json::parse(body, nullptr, false);
    if (j.is_object()) {
        for (const char* k : {"error", "message", "title", "detail"})  // the last one found is the most specific
            if (j.contains(k) && j[k].is_string()) detail = j[k].get<std::string>();
    }
    switch (status) {
        case 401: return std::string("the API key was refused; check ") + kKeyEnvVarText;
        case 429: return "the Nexus API's rate limit is used up; try again later";
        default: return "HTTP " + std::to_string(status) + (detail.empty() ? "" : ": " + detail);
    }
}

json v1_get(const std::string& path) {
    std::string text;
    Http r = send("GET", std::string(kV1) + path,
                  {"apikey: " + api_key(), "Accept: application/json", std::string("Application-Name: ") + kToolName,
                   std::string("Application-Version: ") + kToolVersion},
                  "", [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status != 200) fail(api_error(r.status, r.error_body));
    return json::parse(text);
}

// v3 is the API that can change things. Returns null for a response with no body.
json v3_call(const std::string& method, const std::string& path, const json* body = nullptr) {
    std::vector<std::string> headers = {"apikey: " + api_key(), "Accept: application/json",
                                        std::string("Application-Name: ") + kToolName,
                                        std::string("Application-Version: ") + kToolVersion};
    if (body) headers.push_back("Content-Type: application/json");
    std::string text;
    Http r = send(method, std::string(kV3) + path, headers, body ? body->dump(-1, ' ', false, json::error_handler_t::replace) : "",
                  [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status < 200 || r.status >= 300) fail(api_error(r.status, r.error_body));
    return text.empty() ? json(nullptr) : json::parse(text);
}

json graphql(const std::string& query, const json& variables) {
    json payload = {{"query", query}, {"variables", variables}};
    std::string text;
    Http r = send("POST", kGraphQl, {"Content-Type: application/json", "Accept: application/json"}, payload.dump(),
                  [&](const char* p, size_t n) { text.append(p, n); });
    if (r.status != 200) fail(api_error(r.status, r.error_body));
    json j = json::parse(text);
    if (j.contains("errors") && !j["errors"].empty()) fail("GraphQL error: " + j["errors"].dump());
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

// ---------------------------------------------------------------- commands

std::string read_file(const fs::path& path);

struct Target {
    std::string game;
    std::string mod;
    int limit = kDefaultLimit;
    int offset = 0;

    std::string v1_mod() const { return "/games/" + game + "/mods/" + mod; }
    long long mod_id() const { return std::stoll(mod); }
    json header() const { return {{"mod_id", mod_id()}, {"game", game}}; }
    std::string page() const { return "https://www.nexusmods.com/" + game + "/mods/" + mod; }
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

json file_object(const json& f) {
    int category = f.value("category_id", 0);
    return {{"file_id", value_of(f, "file_id")},
            {"label", clean_text(text_of(f, "name"))},
            {"version", value_of(f, "version")},
            {"category", category_key(category)},
            {"uploaded", value_of(f, "uploaded_time")},
            {"uploaded_timestamp", value_of(f, "uploaded_timestamp")},
            {"note", clean_text(text_of(f, "description"))},
            {"file_name", value_of(f, "file_name")},
            {"size_bytes", value_of(f, "size_in_bytes")},
            {"size_kb", value_of(f, "size_kb")},
            {"primary", value_of(f, "is_primary")}};
}

json cmd_desc(const Target& t) {
    json m = v1_get(t.v1_mod() + ".json");
    json out = t.header();
    out["name"] = value_of(m, "name");
    out["author"] = value_of(m, "author");
    out["uploader"] = value_of(m, "uploaded_by");
    out["version"] = value_of(m, "version");
    out["status"] = value_of(m, "status");
    out["updated"] = value_of(m, "updated_time");
    out["summary"] = clean_text(text_of(m, "summary"));
    out["description"] = clean_text(text_of(m, "description"));
    return out;
}

json cmd_changelog(const Target& t) {
    json log = v1_get(t.v1_mod() + "/changelogs.json");
    json versions = json::array();
    if (log.is_object()) {
        for (auto& [version, entries] : log.items()) {
            json lines = json::array();
            for (const auto& e : entries)
                if (e.is_string()) lines.push_back(clean_text(e.get<std::string>()));
            versions.push_back({{"version", version}, {"entries", lines}});
        }
    }
    json out = t.header();
    out["changelog"] = versions;
    return out;
}

json cmd_files(const Target& t, bool archived) {
    json listing = v1_get(t.v1_mod() + "/files.json");
    std::map<int, std::vector<json>> groups;
    for (const auto& f : listing["files"]) {
        int category = f.value("category_id", 0);
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
            return a.value("uploaded_timestamp", 0LL) > b.value("uploaded_timestamp", 0LL);
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

json cmd_file_note(const Target& t, const std::string& file) {
    json f = v1_get(t.v1_mod() + "/files/" + file + ".json");
    json o = file_object(f);
    json out = t.header();
    for (const char* k : {"file_id", "label", "version", "category", "uploaded", "note"}) out[k] = o[k];
    return out;
}

json cmd_dl_file(const Target& t, const std::string& file, const std::string& dest, bool force) {
    std::string base = t.v1_mod() + "/files/" + file;
    json info = v1_get(base + ".json");
    std::string file_name = fs::u8path(text_of(info, "file_name")).filename().u8string();
    if (file_name.empty()) fail("the API gave no file name for file " + file);

    json links;
    {
        std::string text;
        Http r = send("GET", std::string(kV1) + base + "/download_link.json",
                      {"apikey: " + api_key(), "Accept: application/json", std::string("Application-Name: ") + kToolName,
                       std::string("Application-Version: ") + kToolVersion},
                      "", [&](const char* p, size_t n) { text.append(p, n); });
        if (r.status == 403)
            fail("Nexus refused the download link. API downloads need a Premium account; " + api_error(r.status, r.error_body));
        if (r.status != 200) fail(api_error(r.status, r.error_body));
        links = json::parse(text);
    }
    if (!links.is_array() || links.empty() || !links[0].contains("URI")) fail("the API returned no download link");

    fs::path target = to_wide(dest);
    bool is_folder = fs::is_directory(target) || dest.back() == '/' || dest.back() == '\\';
    if (is_folder) target /= fs::u8path(file_name);
    if (fs::exists(target) && !force) fail(target.u8string() + " already exists (--force replaces it)");
    if (target.has_parent_path()) fs::create_directories(target.parent_path());
    fs::path part = target;
    part += L".part";

    FILE* out_file = _wfopen(part.c_str(), L"wb");
    if (!out_file) fail("cannot write " + part.u8string());
    long long written = 0;
    try {
        Http r = send("GET", links[0]["URI"].get<std::string>(), {}, "", [&](const char* p, size_t n) {
            if (std::fwrite(p, 1, n, out_file) != n) fail("cannot write " + part.u8string() + " (disk full?)");
            written += static_cast<long long>(n);
        });
        std::fclose(out_file);
        out_file = nullptr;
        if (r.status != 200) fail("download failed: HTTP " + std::to_string(r.status));
        if (info.contains("size_in_bytes") && info["size_in_bytes"].is_number_integer() &&
            info["size_in_bytes"].get<long long>() != written)
            fail("download incomplete: got " + std::to_string(written) + " of " +
                 std::to_string(info["size_in_bytes"].get<long long>()) + " bytes");
        fs::rename(part, target);
    } catch (...) {
        if (out_file) std::fclose(out_file);
        std::error_code ignored;
        fs::remove(part, ignored);
        throw;
    }
    json out = t.header();
    out["file_id"] = value_of(info, "file_id");
    out["file_name"] = file_name;
    out["path"] = fs::absolute(target).u8string();
    out["bytes"] = written;
    return out;
}

json ids_variable(const Target& t) {
    return {{"ids", json::array({{{"gameDomain", t.game}, {"modId", t.mod_id()}}})}};
}

json single_mod(const std::string& query, json variables) {
    json nodes = graphql(query, variables)["legacyModsByDomain"]["nodes"];
    if (nodes.empty()) fail("mod not found");
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
    Http put = send("PUT", text_of(upload, "presigned_url"),
                    {"Content-Disposition: attachment; filename=\"" + filename + "\"", "Content-MD5: " + md5_of(data, false),
                     "Content-Type: application/octet-stream"},
                    data, [](const char*, size_t) {});
    if (put.status < 200 || put.status >= 300) fail("sending the file failed: HTTP " + std::to_string(put.status));
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
    std::set<std::string> options;  // besides --game
};

const std::vector<CommandSpec>& command_specs() {
    static const std::set<std::string> paged = {"--limit", "--offset"};
    static const std::vector<CommandSpec> specs = {
        {"desc", 1, {}},
        {"changelog", 1, {}},
        {"files", 1, {}},
        {"archived-files", 1, {}},
        {"dl-file", 2, {"--dest", "--force"}},
        {"file-note", 2, {}},
        {"permissions", 1, {"--html"}},
        {"reqs", 1, paged},
        {"dependents", 1, paged},
        {"list-mods", 1, paged},
        {"save-desc", 1, {"--dest", "--force"}},
        {"set-desc", 1, {"--file", "--backup-dir", "--dry-run", "--allow-html"}},
        {"upload-file", 1, {"--file", "--name", "--file-version", "--replaces", "--category", "--note", "--changelog", "--set-mod-version", "--dry-run", "--allow-html"}},
    };
    return specs;
}

int run(const std::vector<std::string>& args) {
    static const std::set<std::string> with_value = {"--game", "--dest", "--limit", "--offset", "--file", "--backup-dir", "--name",
                                                     "--file-version", "--replaces", "--category", "--note", "--changelog", "--html"};
    static const std::set<std::string> flags = {"--force", "--dry-run", "--allow-html", "--set-mod-version"};
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h" || a == "help") {
            print(kUsage);
            return 0;
        }
        if (a == "--version") {
            print(std::string(kToolName) + " " + kToolVersion + "\n");
            return 0;
        }
        if (with_value.count(a)) {
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
    if (positional.empty()) {
        print(kUsage);
        return 2;
    }
    auto has = [&](const char* k) { return options.count(k) > 0; };
    auto option = [&](const char* k) { return has(k) ? options[k] : std::string(); };
    std::string game = has("--game") ? options["--game"] : kDefaultGame;
    if (!valid_game(game)) bad_args("bad game domain " + game);

    const std::string& command = positional[0];
    const auto& specs = command_specs();
    auto spec = std::find_if(specs.begin(), specs.end(), [&](const CommandSpec& c) { return command == c.name; });
    if (spec == specs.end()) bad_args("unknown command " + command);
    for (const auto& kv : options) {
        if (kv.first != "--game" && !spec->options.count(kv.first)) {
            if (kv.first == "--limit" || kv.first == "--offset") bad_args("--limit and --offset only apply to reqs, dependents and list-mods");
            bad_args(kv.first + " doesn't apply to " + command);
        }
    }

    // permissions also takes a mod page's address
    if (command == "permissions" && positional.size() == 2 && !all_digits(positional[1])) {
        std::string url = positional[1];
        size_t mods = url.find("/mods/");
        size_t host = url.find("nexusmods.com/");
        if (host == std::string::npos || mods == std::string::npos) bad_args("the mod id must be a number or a mod page's address");
        std::string id = url.substr(mods + 6, url.find_first_not_of("0123456789", mods + 6) == std::string::npos ? std::string::npos : url.find_first_not_of("0123456789", mods + 6) - mods - 6);
        size_t from = host + 14;
        std::string domain = url.substr(from, mods - from);
        if (domain.rfind("games/", 0) == 0) domain.erase(0, 6);
        if (!all_digits(id) || !valid_game(domain)) bad_args("that isn't a mod page's address");
        positional[1] = id;
        game = domain;
    }
    bool lists_user = command == "list-mods";
    bool takes_file = spec->positional == 2;
    if (static_cast<int>(positional.size()) != spec->positional + 1)
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
    if (t.limit < 1 || t.limit > kMaxLimit)
        bad_args("--limit must be 1 to " + std::to_string(kMaxLimit) + "; use --offset to read further");

    json result;
    if (command == "desc") result = cmd_desc(t);
    else if (command == "changelog") result = cmd_changelog(t);
    else if (command == "files") result = cmd_files(t, false);
    else if (command == "archived-files") result = cmd_files(t, true);
    else if (command == "dl-file") result = cmd_dl_file(t, positional[2], option("--dest"), has("--force"));
    else if (command == "file-note") result = cmd_file_note(t, positional[2]);
    else if (command == "permissions") result = cmd_permissions(t, option("--html"));
    else if (command == "reqs") result = cmd_reqs(t);
    else if (command == "dependents") result = cmd_dependents(t);
    else if (command == "list-mods") result = cmd_list_mods(t, positional[1]);
    else if (command == "save-desc") result = cmd_save_desc(t, option("--dest"), has("--force"));
    else if (command == "set-desc") result = cmd_set_desc(t, option("--file"), option("--backup-dir"), has("--dry-run"), has("--allow-html"));
    else {
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
        result = cmd_upload_file(t, a);
    }

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
    } catch (const Failure& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return e.code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
