/*
 * Monolith MCP stdio-to-HTTP proxy (C++ / WinHTTP).
 *
 * Sits between Claude Code (stdio JSON-RPC) and Monolith (HTTP on localhost).
 * Handles initialize locally, forwards tool calls to Monolith.
 * Survives editor restarts -- proxy process never dies.
 * Background health poll auto-detects when the editor comes online.
 *
 * Build: see build.bat or CMakeLists.txt
 * Usage (in .mcp.json):
 *   {"mcpServers":{"monolith":{"command":"path/to/monolith_proxy.exe"}}}
 */

// ============================================================================
// Includes
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <vector>
#include <iomanip>
#include <condition_variable>
#include <deque>
#include <functional>
#include <atomic>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;

// ============================================================================
// Constants
// ============================================================================

static const char* PROXY_NAME    = "monolith-proxy";
static const char* PROXY_VERSION = "1.2.0";

static double TIMEOUT = 120.0;
static constexpr double POLL_INTERVAL            = 5.0;
static constexpr double POLL_START_DELAY         = 3.0;


static const std::set<std::string> SUPPORTED_VERSIONS = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"
};

static const std::set<std::string> EDITOR_BUILD_ACTIONS = {
    "trigger_build", "live_compile"
};

static const std::set<std::string> EDITOR_READ_ACTIONS = {
    "get_build_errors",
    "get_build_status",
    "get_build_summary",
    "search_build_output",
    "get_recent_logs",
    "search_logs",
    "tail_log",
    "get_log_categories",
    "get_log_stats",
    "get_compile_output",
    "get_crash_context",
};

// ============================================================================
// Globals
// ============================================================================

static std::string g_monolith_url;        // e.g. "http://localhost:9316/mcp"
static std::string g_monolith_host;       // e.g. "localhost"
static bool        g_monolith_secure = false;
static int         g_monolith_port = 0;   // e.g. 9316
static std::string g_monolith_path_mcp;   // e.g. "/mcp"
static std::string g_monolith_path_health;// e.g. "/health"

static bool g_split_editor_query = false;
static std::set<std::string> g_editor_action_allowlist;
static std::set<std::string> g_editor_action_denylist;

// State tracking
static std::optional<bool> g_monolith_was_up; // nullopt = unknown
static std::mutex g_stdout_lock;
static std::mutex g_cache_lock;
static std::mutex g_poll_lock;
static std::condition_variable g_poll_cv;
static bool g_stopping = false;

// Call-log state (Phase 4 / survivor F)
//
// NOTE: Saved/Logs/MonolithCalls-<pid>.jsonl is project-root-relative and excluded
// from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
// only includes editor logs, not arbitrary jsonl). If a crash collector pattern
// elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls-<pid>.jsonl to
// the exclusion list. Single-user local dev tool; no phone-home.
static bool      g_call_log_enabled = false;     // resolved once at startup
static HANDLE    g_call_log_handle  = INVALID_HANDLE_VALUE;
static std::mutex g_call_log_lock;

static const std::vector<std::string> CORE_QUERY_TOOLS = {
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
};

// ============================================================================
// Logging
// ============================================================================

static void log_msg(const std::string& msg)
{
    std::cerr << "[monolith-proxy] " << msg << std::endl;
}

// ============================================================================
// Utility: environment variable helpers
// ============================================================================

static std::string get_env(const char* name, const char* default_val = "")
{
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
}

static std::set<std::string> parse_csv_env(const char* name)
{
    std::set<std::string> result;
    std::string raw = get_env(name);
    if (raw.empty()) return result;

    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, ','))
    {
        // trim whitespace
        size_t start = part.find_first_not_of(" \t\r\n");
        size_t end   = part.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = part.substr(start, end - start + 1);
        // lowercase
        std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (!trimmed.empty())
            result.insert(std::move(trimmed));
    }
    return result;
}

// ============================================================================
// Utility: URL parsing
// ============================================================================

static void parse_monolith_url(const std::string& url)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), (int)url.size(), nullptr, 0);
    if (size <= 0) throw std::invalid_argument("URL encoding");
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), (int)url.size(), wide.data(), size);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    parts.dwUserNameLength = parts.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength)
        throw std::invalid_argument("Expected http(s)://host:port/mcp without credentials or query");
    auto utf8 = [](const wchar_t* value, DWORD length) {
        if (!length) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
        std::string result(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), n, nullptr, nullptr);
        return result;
    };
    g_monolith_url = url;
    g_monolith_host = utf8(parts.lpszHostName, parts.dwHostNameLength);
    g_monolith_port = parts.nPort;
    g_monolith_secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    g_monolith_path_mcp = parts.dwUrlPathLength ? utf8(parts.lpszUrlPath, parts.dwUrlPathLength) : "/mcp";
    const auto slash = g_monolith_path_mcp.rfind('/');
    g_monolith_path_health = g_monolith_path_mcp.substr(0, slash) + "/health";
}

// ============================================================================
// Time helper
// ============================================================================

static double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// JSONL call log (Phase 4 / survivor F)
//
// One line per upstream HTTP roundtrip:
//   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
//    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
//    "error_code":null,"result_bytes":1834}
//
// Path: <project-root>/Saved/Logs/MonolithCalls-<pid>.jsonl
// Opt-out: env var MONOLITH_CALL_LOG=0
// Append semantics on Win32: CreateFile w/ FILE_APPEND_DATA — OS guarantees
// atomic end-of-file positioning for writes < 4KB. Never seek before write.
// ============================================================================

static std::string sha1_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return std::string();

    DWORD hashObjSize = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &cb, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    std::vector<UCHAR> hashObj(hashObjSize);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    if (BCryptHashData(hHash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
            static_cast<ULONG>(data.size()), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    UCHAR digest[20] = {0};
    if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const char* kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (size_t i = 0; i < 20; ++i)
    {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i]        & 0x0F];
    }
    return out;
}

static std::string iso8601_utc_now()
{
    // Second-precision ISO-8601 UTC, e.g. "2026-05-27T18:14:56Z".
    std::time_t t = std::time(nullptr);
    std::tm tm_buf;
    gmtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

// Resolve <project-root>/Saved/Logs/MonolithCalls-<pid>.jsonl
// Priority:
//   1. MONOLITH_PROJECT_ROOT env var (explicit override)
//   2. Current working directory (proxy CWD is the project root when launched
//      by Claude Code's MCP config)
static std::string resolve_call_log_path()
{
    std::string root = get_env("MONOLITH_PROJECT_ROOT");
    if (root.empty())
    {
        char cwd_buf[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd_buf);
        if (n > 0 && n < MAX_PATH)
            root = std::string(cwd_buf, n);
        else
            root = ".";
    }

    // Strip any trailing slash so we can append uniformly
    while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();

    std::string saved   = root + "\\Saved";
    std::string logsdir = saved + "\\Logs";

    CreateDirectoryA(saved.c_str(), nullptr);    // OK if already exists
    CreateDirectoryA(logsdir.c_str(), nullptr);

    return logsdir + "\\MonolithCalls-" + std::to_string(GetCurrentProcessId()) + ".jsonl";
}

static void init_call_log()
{
    // Default-enabled; only "0" disables. Read once at startup, cache the bool.
    g_call_log_enabled = get_env("MONOLITH_CALL_LOG", "1") != "0";
    if (!g_call_log_enabled)
    {
        log_msg("Call log disabled (MONOLITH_CALL_LOG=0)");
        return;
    }

    std::string path = resolve_call_log_path();
    g_call_log_handle = CreateFileA(
        path.c_str(),
        FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (g_call_log_handle == INVALID_HANDLE_VALUE)
    {
        log_msg("Failed to open call log at " + path + " -- logging disabled");
        g_call_log_enabled = false;
        return;
    }

    log_msg("Call log: " + path);
}

// Canonicalised JSON over a params-style object: sorted keys, no whitespace.
// Matches Python json.dumps(sort_keys=True, separators=(",",":")).
static std::string canonical_json(const json& value)
{
    // nlohmann::json's underlying object is std::map (alphabetically sorted by
    // key); dump(-1) emits compact form (no spaces). For nested objects, the
    // library walks std::map-order recursively. This matches the sorted-keys
    // contract for hashing purposes.
    if (value.is_null())
        return "{}";
    return value.dump(-1);
}

// Extract (namespace, action) from a JSON-RPC message.
//   - tools/call w/ name matching "*_query": namespace = prefix, action = args.action
//   - tools/call w/ name matching "monolith_*": namespace = "monolith", action = suffix
//   - tools/call other: namespace = name, action = ""
//   - non-tools/call (initialize, ping, tools/list): namespace = method, action = ""
static void extract_namespace_action(const json& msg, std::string& ns, std::string& action)
{
    std::string method = msg.value("method", "");
    if (method != "tools/call")
    {
        ns = method;
        action = "";
        return;
    }

    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
    {
        ns = "tools/call";
        action = "";
        return;
    }

    std::string name = params_it->value("name", "");
    const std::string query_suffix = "_query";
    if (name.size() > query_suffix.size() &&
        name.compare(name.size() - query_suffix.size(), query_suffix.size(), query_suffix) == 0)
    {
        ns = name.substr(0, name.size() - query_suffix.size());
        json args = params_it->value("arguments", json::object());
        if (args.is_object())
            action = args.value("action", "");
        else
            action = "";
        return;
    }

    const std::string monolith_prefix = "monolith_";
    if (name.rfind(monolith_prefix, 0) == 0)
    {
        ns = "monolith";
        action = name.substr(monolith_prefix.size());
        return;
    }

    ns = name;
    action = "";
}

// Extract params dict for hashing. For tools/call, that's params.arguments;
// for other methods, that's the whole params object.
static json extract_params_for_hash(const json& msg)
{
    std::string method = msg.value("method", "");
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return json::object();

    if (method == "tools/call")
    {
        json args = params_it->value("arguments", json::object());
        if (!args.is_object())
            return json::object();
        return args;
    }
    return *params_it;
}

// Inspect a forwarded HTTP response to extract (ok, error_code, result_bytes).
//   - ok = true iff response is valid JSON-RPC AND has no top-level "error"
//   - error_code = response.error.code if present, else null
//   - result_bytes = length of serialised result payload (or full response if no result)
static void inspect_response(const std::string& resp, bool& ok,
    std::optional<int>& error_code, size_t& result_bytes)
{
    ok = false;
    error_code.reset();
    result_bytes = 0;

    if (resp.empty())
        return;

    try
    {
        json parsed = json::parse(resp);
        auto err_it = parsed.find("error");
        if (err_it != parsed.end() && err_it->is_object())
        {
            ok = false;
            auto code_it = err_it->find("code");
            if (code_it != err_it->end() && code_it->is_number_integer())
                error_code = code_it->get<int>();
        }
        else
        {
            ok = parsed.is_object() && parsed.value("jsonrpc", "") == "2.0" && parsed.contains("result")
                && !(parsed["result"].is_object() && parsed["result"].value("isError", false));
        }

        auto result_it = parsed.find("result");
        if (result_it != parsed.end())
            result_bytes = result_it->dump(-1).size();
        else
            result_bytes = resp.size();
    }
    catch (...)
    {
        // Unparseable -- treat as failure with no error_code; record full body size
        ok = false;
        result_bytes = resp.size();
    }
}

static void write_call_log_line(const json& msg, const std::string& resp, double duration_ms)
{
    if (!g_call_log_enabled || g_call_log_handle == INVALID_HANDLE_VALUE)
        return;

    try
    {
        std::string ns;
        std::string action;
        extract_namespace_action(msg, ns, action);

        json params_for_hash = extract_params_for_hash(msg);
        std::string canonical = canonical_json(params_for_hash);
        std::string params_hash = sha1_hex(canonical);

        bool ok = false;
        std::optional<int> error_code;
        size_t result_bytes = 0;
        inspect_response(resp, ok, error_code, result_bytes);

        json line;
        line["proxy_pid"] = GetCurrentProcessId();
        line["request_id"] = msg.value("id", json());
        line["ts"]           = iso8601_utc_now();
        line["namespace"]    = ns;
        line["action"]       = action;
        line["params_hash"]  = params_hash;
        line["duration_ms"]  = duration_ms;
        line["ok"]           = ok;
        if (error_code.has_value())
            line["error_code"] = error_code.value();
        else
            line["error_code"] = nullptr;
        line["result_bytes"] = static_cast<int64_t>(result_bytes);

        std::string serialised = line.dump(-1);
        serialised.push_back('\n');

        // FILE_APPEND_DATA guarantees the kernel positions writes at EOF
        // atomically; single WriteFile call keeps the line indivisible up to
        // PIPE_BUF / 4KB. Mutex guards our handle from cross-thread races
        // (defence in depth -- only the dispatcher main thread writes today).
        std::lock_guard<std::mutex> lock(g_call_log_lock);
        DWORD written = 0;
        WriteFile(g_call_log_handle,
            serialised.data(),
            static_cast<DWORD>(serialised.size()),
            &written,
            nullptr);
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Call-log write failed: ") + e.what());
    }
    catch (...)
    {
        // never let logging crash the proxy
    }
}

// ============================================================================
// WinHTTP client
// ============================================================================

static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], sz);
    return ws;
}

// POST JSON to Monolith. Returns response body or empty string on failure.
static std::string post_monolith(const std::string& body, double timeout_sec = TIMEOUT)
{
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return {};

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    std::wstring wpath = to_wide(g_monolith_path_mcp);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, g_monolith_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    // Set timeouts (milliseconds)
    DWORD timeout_ms = (DWORD)(timeout_sec * 1000);
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    DWORD disableFeatures = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disableFeatures, sizeof(disableFeatures));

    // Send
    const wchar_t* hdrs = L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nMCP-Protocol-Version: 2025-03-26";
    BOOL ok = WinHttpSendRequest(
        hRequest, hdrs, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(),
        (DWORD)body.size(), 0);

    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response
    std::string response;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead);
        response.append(chunk.c_str(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    try {
        auto parsed = json::parse(response);
        const auto request = json::parse(body);
        if (!parsed.is_object() || parsed.value("jsonrpc", "") != "2.0" ||
            !parsed.contains("id") || parsed["id"].is_boolean() ||
            parsed.contains("result") == parsed.contains("error")) return {};
        if (parsed.contains("error")) {
            const auto& error = parsed["error"];
            if (!error.is_object() || !error.contains("code") || !error["code"].is_number_integer() ||
                !error.contains("message") || !error["message"].is_string()) return {};
        }
        if (parsed["id"] != request.value("id", json())) {
            // Rejections before parsing have no request ID. Rebind only the
            // explicit invalid-request / never-executed contract.
            if (!parsed["id"].is_null() || !parsed.contains("error")) return {};
            const auto& error = parsed["error"];
            if (error["code"] != -32600 || !error.contains("data") || !error["data"].is_object()) return {};
            const auto& data = error["data"];
            if (!data.contains("executed") || !data["executed"].is_boolean() || data["executed"].get<bool>()) return {};
            parsed["id"] = request.value("id", json());
            response = parsed.dump();
        }
    } catch (...) { return {}; }
    return response;
}

// GET health endpoint. Returns true if 200 OK.
static bool check_monolith_up()
{
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return false;

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    std::wstring wpath = to_wide(g_monolith_path_health);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, g_monolith_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // 3-second timeout for health check
    DWORD timeout_ms = 3000;
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return statusCode == 200;
}

// ============================================================================
// JSON-RPC helpers
// ============================================================================

static std::string make_result(const json& id, const json& result)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp.dump();
}

static std::string make_tool_error(const json& id, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = {
        {"content", json::array({{{"type", "text"}, {"text", message}}})},
        {"isError", true}
    };
    return resp.dump();
}

static std::string make_jsonrpc_error(const json& id, int code, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = {{"code", code}, {"message", message}};
    return resp.dump();
}

// ============================================================================
// Stable tools/list fallback
// ============================================================================

static std::string sanitize_cache_part(std::string value)
{
    for (char& c : value)
    {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_';
        if (!ok)
            c = '_';
    }
    return value;
}

static std::string tools_cache_path()
{
    std::string base = get_env("LOCALAPPDATA");
    if (base.empty())
        base = get_env("TEMP", ".");

    std::string dir = base + "\\Monolith";
    CreateDirectoryA(dir.c_str(), nullptr);

    char cwd[32768];
    const auto n = GetCurrentDirectoryA(sizeof(cwd), cwd);
    const auto project = get_env("MONOLITH_PROJECT_ROOT", n > 0 && n < sizeof(cwd) ? cwd : ".");
    return dir + "\\monolith_proxy_tools_" +
        sanitize_cache_part(g_monolith_host) + "_" +
        std::to_string(g_monolith_port) + "_" + sha1_hex(g_monolith_url + "|" + project).substr(0, 16) + ".json";
}

static json make_query_tool_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"description", "The action to execute. Use monolith_discover first when the editor is available."}
            }},
            {"params", {
                {"type", "object"},
                {"description", "Parameters for the selected action."}
            }},
            {"_fields", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
            }},
            {"_omit", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
            }},
            {"_compact_json", {
                {"type", "boolean"},
                {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
            }}
        }},
        {"required", json::array({"action"})}
    };
}

static json make_empty_object_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"_fields", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
            }},
            {"_omit", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
            }},
            {"_compact_json", {
                {"type", "boolean"},
                {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
            }}
        }}
    };
}

static json make_tool(const std::string& name, const std::string& description, const json& schema)
{
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", schema}
    };
}

static json make_seed_tools()
{
    json tools = json::array();

    for (const std::string& name : CORE_QUERY_TOOLS)
    {
        std::string domain = name;
        const std::string suffix = "_query";
        if (domain.size() > suffix.size() &&
            domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            domain.resize(domain.size() - suffix.size());
        }

        tools.push_back(make_tool(
            name,
            "Query the " + domain + " domain. The editor may be offline at session start; retry after Monolith is healthy.",
            make_query_tool_schema()));
    }

    tools.push_back(make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            {"type", "object"},
            {"properties", {
                {"namespace", {
                    {"type", "string"},
                    {"description", "Optional: filter to a specific namespace"}
                }},
                {"category", {
                    {"type", "string"},
                    {"description", "Optional: filter actions within the namespace by category"}
                }},
                {"_fields", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
                }},
                {"_omit", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
                }},
                {"_compact_json", {
                    {"type", "boolean"},
                    {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        make_empty_object_schema()));

    tools.push_back(make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"description", "'check' to compare versions, 'install' to download and stage update"},
                    {"default", "check"}
                }},
                {"_fields", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."}
                }},
                {"_omit", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."}
                }},
                {"_compact_json", {
                    {"type", "boolean"},
                    {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        make_empty_object_schema()));

    tools.push_back(make_tool(
        "monolith_guide",
        "Read Monolith workflow recipes, onboarding, decisions, and recovery guidance.",
        {{"type", "object"}, {"properties", {
            {"section", {{"type", "string"}, {"description", "Optional section: onboarding, recipes, decisions, errors, skills_map, gotchas. Omit for all sections."}}}
        }}}));
    tools.push_back(make_tool(
        "monolith_coordination",
        "Acquire, renew, release, or inspect an editor-wide workflow lease.",
        {{"type", "object"}, {"properties", {
            {"operation", {{"type", "string"}, {"enum", {"status", "acquire", "renew", "release"}}, {"default", "status"}}},
            {"owner", {{"type", "string"}, {"description", "Required for acquire: nonempty owner label, at most 128 characters."}}},
            {"ttl_seconds", {{"type", "number"}, {"minimum", 10}, {"maximum", 600},
                {"description", "Acquire defaults to 120 seconds; renew retains the current duration when omitted."}}},
            {"_lease_token", {{"type", "string"}, {"description", "Token returned by acquire; required for renew and release."}}}
        }}}));
    for (auto& tool : tools) {
        if (tool["name"].get<std::string>().rfind("monolith_", 0) == 0 &&
            !tool["inputSchema"]["properties"].contains("_lease_token")) {
            tool["inputSchema"]["properties"]["_lease_token"] = {
                {"type", "string"}, {"description", "Optional owner token for a protected core tool call."}};
        }
    }

    return tools;
}

static void write_cache(const std::string& path, const json& value)
{
    std::lock_guard<std::mutex> guard(g_cache_lock);
    try
    {
        const auto temp = path + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
        bool written = false;
        { std::ofstream out(temp, std::ios::binary | std::ios::trunc);
          if (out) { out << value.dump(); out.flush(); written = bool(out); } }
        if (!written) { DeleteFileA(temp.c_str()); return; }
        // Older cache readers / antivirus may omit FILE_SHARE_DELETE. Retry only
        // this local rename, never the editor request. The old complete cache
        // remains available if replacement cannot finish within this short bound.
        DWORD error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            if (MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return;
            error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
            if (attempt < 9) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        DeleteFileA(temp.c_str());
        log_msg("Failed to replace proxy cache (Windows error " + std::to_string(error) + ")");
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to write proxy cache: ") + e.what());
    }
}

static std::optional<json> read_cache(const std::string& path)
{
    std::lock_guard<std::mutex> guard(g_cache_lock);
    try
    {
        // CRT ifstream does not share DELETE access. A concurrent atomic rename
        // can already have published its new name while MoveFileEx still holds
        // its DELETE handle, making CRT reads fail with a sharing violation.
        // Sharing delete lets readers consume that complete snapshot immediately.
        // https://devblogs.microsoft.com/oldnewthing/20211022-00/?p=105822
        struct CacheReadHandle
        {
            HANDLE value;
            ~CacheReadHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } in{CreateFileA(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (in.value == INVALID_HANDLE_VALUE) return std::nullopt;
        std::string contents;
        char buffer[16384];
        DWORD bytes = 0;
        for (;;)
        {
            if (!ReadFile(in.value, buffer, sizeof(buffer), &bytes, nullptr)) return std::nullopt;
            if (bytes == 0) break;
            contents.append(buffer, bytes);
        }
        return json::parse(contents);
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to read proxy cache: ") + e.what());
        return std::nullopt;
    }
}

static void write_tools_cache(const std::string& response)
{
    try {
        const auto payload = json::parse(response);
        const auto result = payload.find("result");
        if (result == payload.end() || !result->is_object()) return;
        const auto tools = result->find("tools");
        if (tools != result->end() && tools->is_array() && !tools->empty())
            write_cache(tools_cache_path(), *tools);
    } catch (const std::exception& e) {
        log_msg(std::string("Failed to parse tools/list cache: ") + e.what());
    }
}

static std::optional<json> read_tools_cache()
{
    auto tools = read_cache(tools_cache_path());
    return tools && tools->is_array() && !tools->empty() ? tools : std::nullopt;
}

static std::string rewrite_tools_list(std::string resp)
{
        if (g_split_editor_query)
        {
            try
            {
                json payload = json::parse(resp);
                auto result_it = payload.find("result");
                if (result_it != payload.end() && result_it->is_object())
                {
                    auto tools_it = result_it->find("tools");
                    if (tools_it != result_it->end() && tools_it->is_array())
                    {
                        json rewritten_tools = json::array();
                        for (auto& tool : *tools_it)
                        {
                            if (tool.is_object() && tool.value("name", "") == "editor_query")
                            {
                                // Create read tool
                                json read_tool = tool;
                                read_tool["name"] = "editor_read_query";
                                read_tool["description"] =
                                    "Read-only Unreal editor diagnostics and log access. "
                                    "Use for build status, build errors, build summary, compile output, crash context, "
                                    "and recent log queries. Never use this tool to trigger a build.";

                                // Create build tool
                                json build_tool = tool;
                                build_tool["name"] = "editor_build_query";
                                build_tool["description"] =
                                    "Mutating Unreal editor build actions only. "
                                    "Use only when the user explicitly asks to trigger a full build or a Live Coding compile.";

                                rewritten_tools.push_back(std::move(read_tool));
                                rewritten_tools.push_back(std::move(build_tool));
                                continue;
                            }
                            rewritten_tools.push_back(tool);
                        }
                        (*result_it)["tools"] = std::move(rewritten_tools);
                        resp = payload.dump();
                    }
                }
            }
            catch (const std::exception& e)
            {
                log_msg(std::string("Failed to rewrite tools/list response: ") + e.what());
            }
        }
    return resp;
}

static std::string make_fallback_tools_list_response(const json& msg)
{
    if (auto cached = read_tools_cache())
    {
        log_msg("Monolith down during tools/list -- returning cached tools");
        return rewrite_tools_list(make_result(msg.value("id", json()), {{"tools", cached.value()}}));
    }

    log_msg("Monolith down during tools/list -- returning seed tools");
    return rewrite_tools_list(make_result(msg.value("id", json()), {{"tools", make_seed_tools()}}));
}

// ============================================================================
// stdout writing (thread-safe)
// ============================================================================

static void write_stdout(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_stdout_lock);
    std::cout << msg << "\n";
    std::cout.flush();
}

// ============================================================================
// State check + health poll
// ============================================================================

static bool send_list_changed()
{
    try
    {
        json notification;
        notification["jsonrpc"] = "2.0";
        notification["method"] = "notifications/tools/list_changed";
        write_stdout(notification.dump());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static void check_monolith_state_change()
{
    bool is_up = check_monolith_up();

    if (g_monolith_was_up.has_value() && is_up != g_monolith_was_up.value())
    {
        const char* direction = is_up ? "online" : "offline";
        log_msg(std::string("Monolith went ") + direction + " -- sending tools/list_changed");
        send_list_changed();
    }

    g_monolith_was_up = is_up;
}

static void health_poll_thread()
{
    std::unique_lock<std::mutex> lock(g_poll_lock);
    if (g_poll_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_stopping; })) return;
    while (!g_stopping) {
        lock.unlock();
        try { check_monolith_state_change(); } catch (...) { log_msg("Health poll error"); }
        lock.lock();
        g_poll_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_stopping; });
    }
}

// ============================================================================
// Handlers
// ============================================================================

static const char* DEFAULT_INSTRUCTIONS =
    "Monolith MCP server for Unreal Engine. "
    "Before calling a domain action, check its schema instead of guessing: "
    "monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a "
    "namespace's action names + descriptions (terse by default — pass detail=true to "
    "inline param schemas), and describe_query('action_schema', ...) returns one action's "
    "exact parameter schema. monolith_guide(section='recipes') gives cross-namespace "
    "workflows, decision matrices, and gotchas. For multi-agent edits acquire monolith_coordination "
    "and pass _lease_token on domain calls; renew before expiry. Transport timeouts have unknown execution outcome.";

static std::string handle_initialize(const json& msg, bool fetch_instructions = true)
{
    std::string client_version = "2025-11-25";
    auto params_it = msg.find("params");
    if (params_it != msg.end() && params_it->is_object())
    {
        auto pv_it = params_it->find("protocolVersion");
        if (pv_it != params_it->end() && pv_it->is_string())
            client_version = pv_it->get<std::string>();
    }

    std::string version = (SUPPORTED_VERSIONS.count(client_version) > 0)
        ? client_version : "2025-11-25";

    json result;
    result["protocolVersion"] = version;
    result["capabilities"] = {{"tools", {{"listChanged", true}}}};
    result["serverInfo"] = {{"name", PROXY_NAME}, {"version", PROXY_VERSION}};
    const auto path = tools_cache_path() + ".instructions";
    auto instructions = read_cache(path);
    if (fetch_instructions)
    {
        // Only metadata is imported. Negotiation and proxy capabilities stay local.
        json upstream = msg;
        upstream["params"] = {{"protocolVersion", version}, {"capabilities", json::object()},
                              {"clientInfo", {{"name", PROXY_NAME}, {"version", PROXY_VERSION}}}};
        const auto response = post_monolith(upstream.dump(), 1.0);
        try {
            const auto payload = json::parse(response);
            const auto upstream_result = payload.find("result");
            if (upstream_result != payload.end() && upstream_result->is_object()) {
                const auto value = upstream_result->find("instructions");
                if (value != upstream_result->end() && value->is_string()) {
                    instructions = *value;
                    write_cache(path, *value);
                }
            }
        } catch (const std::exception&) { /* Keep the last complete local snapshot. */ }
    }
    result["instructions"] = instructions && instructions->is_string()
        ? *instructions : json(DEFAULT_INSTRUCTIONS);

    return make_result(msg.value("id", json()), result);
}

static std::string handle_ping(const json& msg)
{
    return make_result(msg.value("id", json()), json::object());
}

static std::string handle_tools_list(const json& msg)
{
    double t0 = now_seconds();
    std::string resp = post_monolith(msg.dump());
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(msg, resp, duration_ms);

    if (!resp.empty())
    {
        write_tools_cache(resp);
        return rewrite_tools_list(resp);
    }

    return make_fallback_tools_list_response(msg);
}

static std::string handle_tools_call(const json& msg)
{
    json id = msg.value("id", json());

    // Extract params (copy so we can modify)
    json params = msg.value("params", json::object());
    std::string tool_name = params.value("name", "unknown");
    std::string forwarded_name = tool_name;
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();

    // --- Split editor_query handling ---
    if (tool_name == "editor_read_query" || tool_name == "editor_build_query")
    {
        // Validate action arg exists
        std::string action;
        auto action_it = args.find("action");
        if (action_it == args.end() || !action_it->is_string() || action_it->get<std::string>().empty())
        {
            return make_tool_error(id,
                "Tool '" + tool_name + "' requires an 'action' string argument.");
        }
        action = action_it->get<std::string>();

        // Normalize
        std::string normalized = action;
        // trim
        size_t s = normalized.find_first_not_of(" \t\r\n");
        size_t e = normalized.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
        // lowercase
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (tool_name == "editor_read_query" && !EDITOR_READ_ACTIONS.count(normalized))
        {
            return make_tool_error(id,
                "Tool '" + tool_name + "' is read-only. Use the build-capable editor-open preset if you intentionally want '" + action + "'.");
        }
        if (tool_name == "editor_build_query" && !EDITOR_BUILD_ACTIONS.count(normalized))
        {
            // Build sorted action list string
            std::string actions_str;
            for (auto it = EDITOR_BUILD_ACTIONS.begin(); it != EDITOR_BUILD_ACTIONS.end(); ++it)
            {
                if (!actions_str.empty()) actions_str += ", ";
                actions_str += *it;
            }
            return make_tool_error(id,
                "Tool '" + tool_name + "' only supports build actions (" + actions_str + "). "
                "Use 'editor_read_query' for diagnostics and logs.");
        }

        // Remap to editor_query for forwarding
        forwarded_name = "editor_query";
        params["name"] = forwarded_name;
    }
    else if (tool_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (g_split_editor_query && EDITOR_BUILD_ACTIONS.count(normalized))
            {
                return make_tool_error(id,
                    "Generic 'editor_query' is not available in split-editor mode for build actions. "
                    "Use 'editor_build_query' from the build-capable preset for '" + action + "'.");
            }
        }
    }

    // Requests with identical arguments can belong to different agents.
    // Build the message we'll actually forward (with possibly rewritten params)
    json forwarded_msg = msg;
    forwarded_msg["params"] = params;

    // --- Allowlist/denylist check ---
    if (forwarded_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (!g_editor_action_allowlist.empty() && !g_editor_action_allowlist.count(normalized))
            {
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Switch to the build-capable editor-open preset if you want mutating editor actions.");
            }
            if (g_editor_action_denylist.count(normalized))
            {
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Use the build-capable editor-open preset when you intentionally want compile or build actions.");
            }
        }
    }

    // --- Record and forward ---


    double t0 = now_seconds();
    std::string resp = post_monolith(forwarded_msg.dump());
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(forwarded_msg, resp, duration_ms);

    if (!resp.empty())
        return resp;

    return make_tool_error(id,
        "Monolith transport failed for '" + tool_name + "': offline, busy, timeout, or invalid response. "
        "Execution outcome is unknown. Inspect editor state before retrying a mutation; no automatic retry was sent.");
}

// ============================================================================
// Main loop
// ============================================================================

// Fixed worker count and bounded queue: no detached request threads and no
// HTTP waits on stdin. Replies may arrive out of order, correlated by RPC id.
class RequestPool {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<json> queue;
    std::set<std::string> active;
    std::vector<std::thread> workers;
    size_t capacity;
    bool stopping = false;
public:
    RequestPool(size_t count, size_t queued) : capacity(count + queued) {
        for (size_t i = 0; i < count; ++i) workers.emplace_back([this] {
            while (true) {
                json msg;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [this] { return stopping || !queue.empty(); });
                    if (queue.empty()) return;
                    msg = std::move(queue.front()); queue.pop_front();
                }
                std::string response;
                try {
                    const auto method = msg.value("method", "");
                    if (method == "tools/call") response = handle_tools_call(msg);
                    else if (method == "tools/list") response = handle_tools_list(msg);
                    else if (method == "initialize") response = handle_initialize(msg);
                    else response = make_jsonrpc_error(msg["id"], -32601, "Method not found: " + method);
                } catch (const std::exception& e) {
                    log_msg(std::string("Request error: ") + e.what());
                    response = make_jsonrpc_error(msg["id"], -32603, "Internal proxy error; execution outcome unknown");
                }
                write_stdout(response);
                std::lock_guard<std::mutex> lock(mutex);
                active.erase(msg["id"].dump());
            }
        });
    }
    bool contains_id(const json& id) {
        std::lock_guard<std::mutex> lock(mutex);
        return active.count(id.dump()) != 0;
    }
    int submit(const json& msg) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto key = msg["id"].dump();
        if (active.count(key)) return -32600;
        if (active.size() >= capacity) return -32001;
        active.insert(key); queue.push_back(msg); ready.notify_one(); return 0;
    }
    ~RequestPool() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_all();
        for (auto& worker : workers) worker.join();
    }
};

static int env_int(const char* name, int fallback, int minimum, int maximum) {
    try {
        size_t end = 0;
        const auto raw = get_env(name);
        const int value = std::stoi(raw, &end);
        if (end == raw.size() && value >= minimum && value <= maximum) return value;
    } catch (...) {}
    return fallback;
}

int main()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    try { parse_monolith_url(get_env("MONOLITH_URL", "http://localhost:9316/mcp")); }
    catch (...) { log_msg("Invalid MONOLITH_URL"); return 1; }
    TIMEOUT = env_int("MONOLITH_TIMEOUT_SECONDS", 120, 1, 3600);
    g_split_editor_query = get_env("MONOLITH_SPLIT_EDITOR_QUERY", "0") == "1";
    g_editor_action_allowlist = parse_csv_env("MONOLITH_EDITOR_ACTION_ALLOWLIST");
    g_editor_action_denylist = parse_csv_env("MONOLITH_EDITOR_ACTION_DENYLIST");
    log_msg("Started. Forwarding to " + g_monolith_url);
    init_call_log();
    std::thread poller(health_poll_thread);
    {
        RequestPool pool(env_int("MONOLITH_MAX_IN_FLIGHT", 8, 1, 32),
                         env_int("MONOLITH_MAX_QUEUED", 64, 0, 1024));
        RequestPool metadata_pool(1, 0); // One metadata request, independent of editor workers.
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            json msg;
            try { msg = json::parse(line); }
            catch (...) { write_stdout(make_jsonrpc_error(nullptr, -32700, "Invalid JSON")); continue; }
            if (!msg.is_object() || !msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0" ||
                !msg.contains("method") || !msg["method"].is_string() ||
                (msg.contains("id") && !msg["id"].is_string() && !msg["id"].is_number())) {
                write_stdout(make_jsonrpc_error(nullptr, -32600, "Invalid JSON-RPC request")); continue;
            }
            if (!msg.contains("id")) continue; // notifications never execute tools
            if (msg.contains("params") && !msg["params"].is_object()) {
                write_stdout(make_jsonrpc_error(msg["id"], -32602, "params must be an object")); continue;
            }
            const auto method = msg.value("method", "");
            if (method == "initialize") {
                // Stdin is the only submitter; workers can only remove active IDs.
                const int code = pool.contains_id(msg["id"]) ? -32600 : metadata_pool.submit(msg);
                if (code == -32600) write_stdout(make_jsonrpc_error(msg["id"], code, "Request id already in flight"));
                else if (code) write_stdout(handle_initialize(msg, false));
                continue;
            }
            if (method == "ping") { write_stdout(handle_ping(msg)); continue; }
            if (method == "tools/call") {
                const auto params = msg.value("params", json::object());
                if (!params.contains("name") || !params["name"].is_string() ||
                    (params.contains("arguments") && !params["arguments"].is_object())) {
                    write_stdout(make_jsonrpc_error(msg["id"], -32602, "name must be a string; arguments must be an object")); continue;
                }
            }
            const int code = metadata_pool.contains_id(msg["id"]) ? -32600 : pool.submit(msg);
            if (code) write_stdout(make_jsonrpc_error(msg["id"], code, code == -32001
                ? "Proxy queue full; request not executed. Retry with backoff." : "Request id already in flight"));
        }
    } // Drain accepted requests before stdout and log handles close.
    { std::lock_guard<std::mutex> lock(g_poll_lock); g_stopping = true; }
    g_poll_cv.notify_all(); poller.join();
    if (g_call_log_handle != INVALID_HANDLE_VALUE) CloseHandle(g_call_log_handle);
    return 0;
}
