// nexus-api-tool: read a Nexus Mods page's information from the command line. See README.md.
//
// Sources: the v1 REST API (needs an API key) for description, changelog and files, the public v2 GraphQL API for
// requirements and dependents. Everything is printed as JSON (UTF-8) on stdout; errors go to stderr.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
constexpr const char* kToolVersion = "0.1.0";
constexpr const char* kDefaultGame = "skyrimspecialedition";
constexpr const char* kV1 = "https://api.nexusmods.com/v1";
constexpr const char* kGraphQl = "https://api.nexusmods.com/v2/graphql";
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
  permissions <mod>                   what the API can tell about permissions (see README)
  reqs <mod>                          what this mod requires
  dependents <mod>                    the mods that require this mod
                                      reqs and dependents return 50 entries by default, at most 200
                                      per call; use next_offset from the output with --offset

OPTIONS
  --game <domain>   the game's Nexus domain (default: skyrimspecialedition)
  --limit <n>       reqs, dependents: entries to return (default 50, at most 200)
  --offset <n>      reqs, dependents: entries to skip (default 0)
  --help, --version

API KEY
  desc, changelog, files, archived-files, dl-file and file-note read your personal API key from the
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
        if (result.status == 200)
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
        for (const char* k : {"message", "error"})
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
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\r') {
            ++i;
        } else if (c == '<' && lower(s.substr(i, 3)) == "<br") {
            size_t end = s.find('>', i);
            if (end == std::string::npos) {
                out += s.substr(i);
                break;
            }
            if (out.empty() || out.back() != '\n') out += '\n';
            i = end + 1;
        } else if (c == '&') {
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

json cmd_permissions(const Target& t) {
    json mod = single_mod("query($ids:[CompositeDomainWithIdInput!]!){legacyModsByDomain(ids:$ids){nodes{name author description uploader{name about}}}}",
                          ids_variable(t));
    json mentions = json::array();
    add_mentions(mentions, "description", clean_text(text_of(mod, "description")));
    if (mod.contains("uploader") && mod["uploader"].is_object())
        add_mentions(mentions, "uploader_profile", clean_text(text_of(mod["uploader"], "about")));
    json out = t.header();
    out["name"] = value_of(mod, "name");
    out["author"] = value_of(mod, "author");
    out["uploader"] = mod["uploader"].is_object() ? value_of(mod["uploader"], "name") : json(nullptr);
    out["checklist"] = nullptr;
    out["checklist_note"] = "The Permissions and credits checklist is only on the mod page; no Nexus API returns it. See page_url.";
    out["page_url"] = t.page() + "?tab=description";
    out["mentions"] = mentions;
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

int run(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    std::string game = kDefaultGame, dest;
    bool has_dest = false, force = false;
    std::string limit_text, offset_text;
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
        if (a == "--game" || a == "--dest" || a == "--limit" || a == "--offset") {
            if (i + 1 >= args.size()) bad_args(a + " needs a value");
            (a == "--game" ? game : a == "--dest" ? dest : a == "--limit" ? limit_text : offset_text) = args[++i];
            if (a == "--dest") has_dest = true;
        } else if (a == "--force") {
            force = true;
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
    if (!valid_game(game)) bad_args("bad game domain " + game);

    const std::string& command = positional[0];
    bool takes_file = command == "dl-file" || command == "file-note";
    static const char* commands[] = {"desc", "changelog", "files", "archived-files", "dl-file", "file-note", "permissions", "reqs", "dependents"};
    if (std::none_of(std::begin(commands), std::end(commands), [&](const char* c) { return command == c; }))
        bad_args("unknown command " + command);
    if (positional.size() != (takes_file ? 3u : 2u))
        bad_args(command + (takes_file ? " needs a mod id and a file number" : " needs a mod id"));
    if (!all_digits(positional[1])) bad_args("the mod id must be a number");
    if (takes_file && !all_digits(positional[2])) bad_args("the file number must be a number");
    if (command == "dl-file" && (!has_dest || dest.empty())) bad_args("dl-file needs --dest <path>");

    Target t{game, positional[1]};
    if (!limit_text.empty() || !offset_text.empty()) {
        if (command != "reqs" && command != "dependents") bad_args("--limit and --offset only apply to reqs and dependents");
        if (!limit_text.empty() && (!all_digits(limit_text) || limit_text.size() > 9)) bad_args("--limit must be a number");
        if (!offset_text.empty() && (!all_digits(offset_text) || offset_text.size() > 9)) bad_args("--offset must be a number");
        if (!offset_text.empty()) t.offset = std::stoi(offset_text);
        if (!limit_text.empty()) t.limit = std::stoi(limit_text);
        if (t.limit < 1 || t.limit > kMaxLimit)
            bad_args("--limit must be 1 to " + std::to_string(kMaxLimit) + "; use --offset to read further");
    }
    json result;
    if (command == "desc") result = cmd_desc(t);
    else if (command == "changelog") result = cmd_changelog(t);
    else if (command == "files") result = cmd_files(t, false);
    else if (command == "archived-files") result = cmd_files(t, true);
    else if (command == "dl-file") result = cmd_dl_file(t, positional[2], dest, force);
    else if (command == "file-note") result = cmd_file_note(t, positional[2]);
    else if (command == "permissions") result = cmd_permissions(t);
    else if (command == "reqs") result = cmd_reqs(t);
    else result = cmd_dependents(t);

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
