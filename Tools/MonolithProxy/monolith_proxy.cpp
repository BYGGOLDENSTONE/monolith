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
#include <stdexcept>
#include <fstream>
#include <vector>
#include <iomanip>

#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;

// ============================================================================
// Constants
// ============================================================================

static const char* PROXY_NAME    = "monolith-proxy";
static const char* PROXY_VERSION = "1.1.1";

static constexpr double POLL_INTERVAL            = 5.0;
static constexpr double POLL_START_DELAY         = 3.0;
static constexpr double REPEAT_TOOL_CALL_WINDOW  = 3.0;

// Timeout configuration (Phase 1 / honest-timeout).
//
// The default stays at 30 s on purpose: the editor's MCP server runs every
// action on a single thread, so waiting longer blocks every other caller too.
// The right fix for genuinely long work is the job system (`jobs` namespace),
// not a bigger wall-clock budget. The env vars exist for the actions that have
// not been converted to jobs yet.
static constexpr double DEFAULT_TIMEOUT               = 30.0;
static constexpr double DEFAULT_TIMEOUT_RESEND_GUARD  = 60.0;

// Classification of an upstream POST failure. A timeout and a dead socket are
// NOT the same event and must never produce the same message.
enum class EPostFailure
{
    None,
    Timeout,      // editor alive but slow -- the request is probably still executing
    Unreachable,  // nothing answered the socket -- the request never ran
};

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
static int         g_monolith_port = 0;   // e.g. 9316
static std::string g_monolith_path_mcp;   // e.g. "/mcp"
static std::string g_monolith_path_health;// e.g. "/health"

static bool g_split_editor_query = false;
static std::set<std::string> g_editor_action_allowlist;
static std::set<std::string> g_editor_action_denylist;

static double g_timeout              = DEFAULT_TIMEOUT;
static double g_timeout_resend_guard = DEFAULT_TIMEOUT_RESEND_GUARD;

// State tracking
static std::optional<bool> g_monolith_was_up; // nullopt = unknown
static std::mutex g_stdout_lock;
static std::unordered_map<std::string, double> g_recent_tool_calls;
// Signature -> timestamp of the call that timed out. Used only to refuse an
// identical resend; the proxy itself never retries anything.
static std::unordered_map<std::string, double> g_timed_out_tool_calls;

// Call-log state (Phase 4 / survivor F)
//
// NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
// from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
// only includes editor logs, not arbitrary jsonl). If a crash collector pattern
// elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
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

// Seconds formatting -- kept byte-identical to the Python proxy so the two
// implementations emit the same text.
// Matches Python's f"{value:g}" -- "30", "60", "1.5".
static std::string fmt_seconds(double value)
{
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

// Matches Python's f"{value:.0f}".
static std::string fmt_whole_seconds(double value)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(0) << value;
    return ss.str();
}

// Read a seconds-valued env var. Falls back to `default_val` when unset,
// unparseable or out of range. `allow_zero` lets a knob be switched off.
static double get_env_seconds(const char* name, double default_val, bool allow_zero = false)
{
    std::string raw = get_env(name);
    // trim
    size_t start = raw.find_first_not_of(" \t\r\n");
    size_t end   = raw.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return default_val;
    raw = raw.substr(start, end - start + 1);

    try
    {
        size_t consumed = 0;
        double value = std::stod(raw, &consumed);
        if (consumed != raw.size())
            throw std::invalid_argument("trailing characters");
        if (value < 0.0 || (value == 0.0 && !allow_zero))
            throw std::out_of_range("out of range");
        return value;
    }
    catch (const std::exception&)
    {
        std::cerr << "[monolith-proxy] Invalid " << name << "='" << raw
                  << "' -- using default " << default_val << std::endl;
        return default_val;
    }
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
    g_monolith_url = url;

    // Strip "http://"
    std::string rest = url;
    if (rest.rfind("http://", 0) == 0)
        rest = rest.substr(7);
    else if (rest.rfind("https://", 0) == 0)
        rest = rest.substr(8);

    // Split host:port/path
    auto slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos) ? rest.substr(0, slash_pos) : rest;
    g_monolith_path_mcp = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "/mcp";

    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos)
    {
        g_monolith_host = host_port.substr(0, colon_pos);
        g_monolith_port = std::stoi(host_port.substr(colon_pos + 1));
    }
    else
    {
        g_monolith_host = host_port;
        g_monolith_port = 80;
    }

    // Derive health path: replace trailing /mcp with /health
    g_monolith_path_health = g_monolith_path_mcp;
    auto mcp_pos = g_monolith_path_health.rfind("/mcp");
    if (mcp_pos != std::string::npos)
        g_monolith_path_health = g_monolith_path_health.substr(0, mcp_pos) + "/health";
    else
        g_monolith_path_health = "/health";
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
// Path: <project-root>/Saved/Logs/MonolithCalls.jsonl
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

// Resolve <project-root>/Saved/Logs/MonolithCalls.jsonl
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

    return logsdir + "\\MonolithCalls.jsonl";
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
            ok = true;
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

// Map a WinHTTP error code onto the two honest outcomes.
static EPostFailure classify_winhttp_error(DWORD err)
{
    return (err == ERROR_WINHTTP_TIMEOUT) ? EPostFailure::Timeout : EPostFailure::Unreachable;
}

// POST JSON to Monolith. Returns response body, or empty string on failure with
// *out_failure describing WHY (timeout vs unreachable -- never conflate them).
//
// Exactly ONE attempt is made. This function has never retried and must never
// start: retrying a timed-out request stacks duplicate work onto the
// single-threaded editor server.
static std::string post_monolith(const std::string& body,
                                 EPostFailure* out_failure = nullptr,
                                 double timeout_sec = -1.0)
{
    if (out_failure) *out_failure = EPostFailure::None;
    if (timeout_sec <= 0.0) timeout_sec = g_timeout;

    auto fail = [out_failure, &timeout_sec](EPostFailure why) -> std::string
    {
        if (out_failure) *out_failure = why;
        if (why == EPostFailure::Timeout)
            log_msg("Monolith timed out after " + fmt_seconds(timeout_sec) + "s");
        else
            log_msg("Monolith unreachable");
        return {};
    };

    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return fail(EPostFailure::Unreachable);

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return fail(EPostFailure::Unreachable); }

    std::wstring wpath = to_wide(g_monolith_path_mcp);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return fail(EPostFailure::Unreachable);
    }

    // Set timeouts (milliseconds)
    DWORD timeout_ms = (DWORD)(timeout_sec * 1000);
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    // Send
    const wchar_t* hdrs = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(
        hRequest, hdrs, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(),
        (DWORD)body.size(), 0);

    DWORD err = ok ? 0 : GetLastError();
    if (ok && !WinHttpReceiveResponse(hRequest, nullptr))
    {
        ok = FALSE;
        err = GetLastError();
    }

    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return fail(classify_winhttp_error(err));
    }

    // Read response. A stall mid-body is a timeout too -- surface it instead of
    // handing the client a truncated payload.
    std::string response;
    bool read_ok = true;
    DWORD read_err = 0;
    for (;;)
    {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
        {
            read_ok = false;
            read_err = GetLastError();
            break;
        }
        if (bytesAvailable == 0)
            break;

        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead))
        {
            read_ok = false;
            read_err = GetLastError();
            break;
        }
        response.append(chunk.c_str(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!read_ok)
        return fail(classify_winhttp_error(read_err));

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
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
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

    return dir + "\\monolith_proxy_tools_" +
        sanitize_cache_part(g_monolith_host) + "_" +
        std::to_string(g_monolith_port) + ".json";
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

    return tools;
}

static void write_tools_cache(const std::string& response)
{
    try
    {
        json payload = json::parse(response);
        auto result_it = payload.find("result");
        if (result_it == payload.end() || !result_it->is_object())
            return;

        auto tools_it = result_it->find("tools");
        if (tools_it == result_it->end() || !tools_it->is_array() || tools_it->empty())
            return;

        std::ofstream out(tools_cache_path(), std::ios::binary | std::ios::trunc);
        if (out)
            out << tools_it->dump();
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to write tools/list cache: ") + e.what());
    }
}

static std::optional<json> read_tools_cache()
{
    try
    {
        std::ifstream in(tools_cache_path(), std::ios::binary);
        if (!in)
            return std::nullopt;

        json tools;
        in >> tools;
        if (!tools.is_array() || tools.empty())
            return std::nullopt;

        return tools;
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to read tools/list cache: ") + e.what());
        return std::nullopt;
    }
}

static std::string make_fallback_tools_list_response(const json& msg)
{
    if (auto cached = read_tools_cache())
    {
        log_msg("Monolith down during tools/list -- returning cached tools");
        return make_result(msg.value("id", json()), {{"tools", cached.value()}});
    }

    log_msg("Monolith down during tools/list -- returning seed tools");
    return make_result(msg.value("id", json()), {{"tools", make_seed_tools()}});
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
// Dedup tracking
// ============================================================================

static std::string tool_signature(const json& msg)
{
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return {};

    auto name_it = params_it->find("name");
    if (name_it == params_it->end() || !name_it->is_string() || name_it->get<std::string>().empty())
        return {};

    // Build signature object using json (std::map-backed, sorts keys alphabetically)
    // This matches Python's json.dumps(sort_keys=True, separators=(",",":"))
    json sig;
    sig["name"] = *name_it;
    sig["arguments"] = params_it->value("arguments", json::object());

    // dump(-1) = compact, no spaces — matches Python separators=(",",":")
    return sig.dump(-1);
}

static bool is_repeated_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (sig.empty()) return false;

    auto it = g_recent_tool_calls.find(sig);
    if (it == g_recent_tool_calls.end()) return false;

    return (now_seconds() - it->second) < REPEAT_TOOL_CALL_WINDOW;
}

static void record_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (!sig.empty())
        g_recent_tool_calls[sig] = now_seconds();
}

// ============================================================================
// Honest failure messages (Phase 1 / honest-timeout)
//
// These strings are kept word-for-word identical to Scripts/monolith_proxy.py.
// If you edit one, edit the other in the same commit.
// ============================================================================

static std::string timeout_message(const std::string& tool_name)
{
    const std::string t = fmt_seconds(g_timeout);
    return
        "Monolith did not answer within " + t + "s. This is a TIMEOUT, not a "
        "disconnection: the Unreal Editor is probably still running and still working on "
        "'" + tool_name + "' right now, and its result will be discarded when it finishes.\n"
        "Do NOT repeat this call. The editor server is single-threaded, so a retry queues a "
        "second copy of the same work behind the first one and makes everything slower. This "
        "proxy did not retry it for you.\n"
        "To follow the work instead of waiting for it: call jobs_query with action 'list' to see "
        "running jobs and their job_id, then jobs_query action 'poll' with that job_id. This "
        "proxy does not know a job_id for the call that timed out -- use 'list' to find it. If "
        "'list' shows nothing, this action has not been converted to a job yet; wait and check "
        "monolith_status before calling anything else.\n"
        "Already converted: animation_query action 'rebuild_pose_search_index' is async by "
        "default and returns a job_id immediately (pass wait=true for the old blocking call).\n"
        "If an unconverted action legitimately needs longer, raise the proxy timeout with the "
        "MONOLITH_TIMEOUT environment variable (seconds, currently " + t + ") and restart the proxy.";
}

static std::string timeout_resend_message(const std::string& tool_name, double age)
{
    double remaining = g_timeout_resend_guard - age;
    if (remaining < 0.0) remaining = 0.0;
    return
        "Tool '" + tool_name + "' with these exact arguments timed out " + fmt_whole_seconds(age) +
        "s ago and was NOT retried. The editor is probably still working on that first request, so "
        "sending it again would queue duplicate work on the single-threaded editor server. Check "
        "progress with jobs_query action 'list' (then action 'poll' with the job_id it reports), or "
        "wait -- this guard expires " + fmt_whole_seconds(remaining) + "s from now. Set "
        "MONOLITH_TIMEOUT_RESEND_GUARD=0 to disable it.";
}

static std::string unreachable_message(const std::string& tool_name)
{
    return
        "Monolith MCP is not available (Unreal Editor not running). The connection was refused "
        "or closed, so tool '" + tool_name + "' did NOT execute -- this is a connection failure, "
        "not a timeout, and nothing is running in the background. Start the editor and try again.";
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
    // Initial delay
    std::this_thread::sleep_for(
        std::chrono::milliseconds((int)(POLL_START_DELAY * 1000)));
    log_msg("Health poll started (interval=" + std::to_string((int)POLL_INTERVAL) + "s)");

    while (true)
    {
        try
        {
            check_monolith_state_change();
        }
        catch (...)
        {
            log_msg("Health poll error");
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds((int)(POLL_INTERVAL * 1000)));
    }
}

// ============================================================================
// Handlers
// ============================================================================

static std::string handle_initialize(const json& msg)
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
    result["instructions"] =
        "Monolith MCP proxy. Tools are forwarded to the Unreal Editor. "
        "If tools return errors about the editor not running, wait and retry.";

    return make_result(msg.value("id", json()), result);
}

static std::string handle_ping(const json& msg)
{
    return make_result(msg.value("id", json()), json::object());
}

static std::string handle_tools_list(const json& msg)
{
    double t0 = now_seconds();
    EPostFailure failure = EPostFailure::None;
    std::string resp = post_monolith(msg.dump(), &failure);
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(msg, resp, duration_ms);

    if (resp.empty() && failure == EPostFailure::Timeout)
        log_msg("tools/list timed out (editor busy, not down) -- serving cached/seed list");

    if (!resp.empty())
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
        write_tools_cache(resp);
        return resp;
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

        if (tool_name == "editor_read_query" && EDITOR_BUILD_ACTIONS.count(normalized))
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

    // --- Dedup check ---
    // Build the message we'll actually forward (with possibly rewritten params)
    json forwarded_msg = msg;
    forwarded_msg["params"] = params;

    // --- Timed-out resend guard ---
    // The proxy does not retry. This stops the *client* from blindly resending
    // a call that is probably still executing inside the editor. It is separate
    // from the 3 s dedup window below, which is far too short to cover a call
    // that already burned the whole timeout budget.
    std::string timeout_sig = tool_signature(forwarded_msg);
    if (!timeout_sig.empty() && g_timeout_resend_guard > 0.0)
    {
        auto it = g_timed_out_tool_calls.find(timeout_sig);
        if (it != g_timed_out_tool_calls.end())
        {
            double age = now_seconds() - it->second;
            if (age < g_timeout_resend_guard)
            {
                log_msg("Refusing resend of '" + tool_name + "' -- it timed out " +
                        fmt_whole_seconds(age) + "s ago");
                return make_tool_error(id, timeout_resend_message(tool_name, age));
            }
            g_timed_out_tool_calls.erase(it);
        }
    }

    if (is_repeated_tool_call(forwarded_msg))
    {
        return make_tool_error(id,
            "Tool '" + tool_name + "' with the same arguments was just called. "
            "Reuse the previous result and answer the user instead of repeating the same call.");
    }

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
    record_tool_call(forwarded_msg);

    double t0 = now_seconds();
    EPostFailure failure = EPostFailure::None;
    std::string resp = post_monolith(forwarded_msg.dump(), &failure);
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(forwarded_msg, resp, duration_ms);

    if (!resp.empty())
    {
        if (!timeout_sig.empty())
            g_timed_out_tool_calls.erase(timeout_sig);
        return resp;
    }

    if (failure == EPostFailure::Timeout)
    {
        if (!timeout_sig.empty())
            g_timed_out_tool_calls[timeout_sig] = now_seconds();
        return make_tool_error(id, timeout_message(tool_name));
    }

    return make_tool_error(id, unreachable_message(tool_name));
}

// ============================================================================
// Main loop
// ============================================================================

int main()
{
    // Binary-safe stdin/stdout on Windows
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Parse configuration from environment
    std::string url = get_env("MONOLITH_URL", "http://localhost:9316/mcp");
    parse_monolith_url(url);

    g_split_editor_query   = get_env("MONOLITH_SPLIT_EDITOR_QUERY", "0") == "1";
    g_editor_action_allowlist = parse_csv_env("MONOLITH_EDITOR_ACTION_ALLOWLIST");
    g_editor_action_denylist  = parse_csv_env("MONOLITH_EDITOR_ACTION_DENYLIST");

    g_timeout              = get_env_seconds("MONOLITH_TIMEOUT", DEFAULT_TIMEOUT);
    // 0 disables the guard entirely.
    g_timeout_resend_guard = get_env_seconds("MONOLITH_TIMEOUT_RESEND_GUARD",
                                             DEFAULT_TIMEOUT_RESEND_GUARD, /*allow_zero=*/true);

    log_msg(std::string("Started. Forwarding to ") + g_monolith_url);

    init_call_log();

    // Start background health poll thread (detached = daemon)
    std::thread poller(health_poll_thread);
    poller.detach();

    // Main stdin read loop
    std::string line;
    while (std::getline(std::cin, line))
    {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;

        // Parse JSON
        json msg;
        try
        {
            msg = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            log_msg(std::string("Bad JSON: ") + e.what());
            continue;
        }

        std::string method = msg.value("method", "");
        bool has_id = msg.contains("id");
        std::string response;

        if (method == "initialize")
        {
            response = handle_initialize(msg);
            log_msg("Initialized");
        }
        else if (method == "notifications/initialized" || method == "initialized")
        {
            // Notification -- no response. Check if Monolith is up.
            check_monolith_state_change();
        }
        else if (method == "ping")
        {
            response = handle_ping(msg);
        }
        else if (method == "tools/list")
        {
            check_monolith_state_change();
            response = handle_tools_list(msg);
        }
        else if (method == "tools/call")
        {
            response = handle_tools_call(msg);
        }
        else
        {
            // Forward unknown methods to Monolith
            double t0 = now_seconds();
            EPostFailure failure = EPostFailure::None;
            std::string resp = post_monolith(msg.dump(), &failure);
            double duration_ms = (now_seconds() - t0) * 1000.0;
            write_call_log_line(msg, resp, duration_ms);

            if (!resp.empty())
            {
                response = resp;
            }
            else if (has_id)
            {
                if (failure == EPostFailure::Timeout)
                {
                    // Reporting "method not found" for a timeout is the same
                    // lie as reporting "editor closed" -- the editor answered
                    // nothing in time, it did not reject the method.
                    response = make_jsonrpc_error(msg["id"], -32000, timeout_message(method));
                }
                else
                {
                    response = make_jsonrpc_error(msg["id"], -32601,
                        "Method not found: " + method);
                }
            }
            // else: notification with no id, silently drop
        }

        if (!response.empty())
            write_stdout(response);
    }

    // EOF on stdin -- clean exit
    log_msg("stdin closed, exiting");
    return 0;
}
