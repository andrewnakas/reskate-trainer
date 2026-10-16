#include <Windows.h>
#include <DbgHelp.h>
#include "Engine/Core/Debug/backtrace.h"
#include "Engine/Core/Debug/native_dump.h"
#include "Engine/Core/Debug/upload.h"
#include "Engine/Core/Debug/multipart.h"
#include "Engine/Core/Debug/protocol.h"
#include "backtrace_config.h"
#include "Engine/Core/Log/logging.h"
#include <fstream>
#include <format>
#include <iostream>
#include <thread>

using namespace dingosdk;
using namespace dingosdk::backtrace;
namespace {
constexpr DWORD test_exception = 0xe0424242;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LONG WINAPI finish_child(EXCEPTION_POINTERS*) { ExitProcess(42); }
std::filesystem::path native_fixture_directory;
LONG WINAPI native_child_handler(EXCEPTION_POINTERS* pointers) {
    const auto path = native_fixture_directory / L"game-crash.mdmp";
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
    if (file.value == INVALID_HANDLE_VALUE || !MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            file.value, MiniDumpNormal, &exception, nullptr, nullptr)) ExitProcess(43);
    CloseHandle(file.release());
    CopyFileW(path.c_str(), (native_fixture_directory / L"duplicate.dmp").c_str(), TRUE);
    ExitProcess(42);
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void endpoint_checks() {
    Endpoint endpoint;
    check(parse_endpoint(L"https://submit.backtrace.io/team/token/minidump", endpoint), "Hosted endpoint rejected");
    check(endpoint.host == L"submit.backtrace.io" && endpoint.port == 443 && endpoint.resource == L"/team/token/minidump", "Hosted URL components incorrect");
    check(parse_endpoint(L"https://team.sp.backtrace.io:6098/post?format=minidump&token=test", endpoint), "Listener endpoint rejected");
    check(endpoint.port == 6098 && endpoint.resource == L"/post?format=minidump&token=test", "Listener query lost");
    for (const auto* value : {L"", L"http://example.com/post", L"https://user:password@example.com/post",
            L"https://example.com/post#fragment", L"https://example.com/post\r\nheader", L"https:///post", L"https://example.com"})
        check(!parse_endpoint(value, endpoint), "Unsafe endpoint accepted");
    std::string id;
    check(accepted_response(200, R"({"response":"ok","_rxid":"1234-abcd"})", id) && id == "1234-abcd", "Valid receipt rejected");
    for (const auto* response : {R"({"response":"error","_rxid":"1234"})", R"({"response":"ok"})", "<html>OK</html>", R"({"response":"ok","_rxid":"bad\nvalue"})"})
        check(!accepted_response(200, response, id), "Unconfirmed submission accepted");
    check(!accepted_response(429, R"({"response":"ok","_rxid":"1234"})", id), "Rate limited submission accepted");
    check(!accepted_response(500, R"({"response":"ok","_rxid":"1234"})", id), "Failed submission accepted");
}
void attachment_checks(const std::filesystem::path& root) {
    const auto directory = root / L"attachments";
    std::filesystem::create_directories(directory);
    const auto log = directory / L"ReSkate.log", dump = directory / L"fixture.dmp";
    const std::string dump_bytes("MDMP\0binary\xff", 12);
    const std::string original_log = "session start\r\nlast crash message\r\n";
    std::ofstream(dump, std::ios::binary).write(dump_bytes.data(), dump_bytes.size());
    std::ofstream(log, std::ios::binary) << original_log;
    check(snapshot_log(log, dump) == LogSnapshot::complete, "Log snapshot failed");
    std::ofstream(log) << "a later game session";
    check(read(log_attachment_path(dump)) == original_log, "Queued attachment changed with live log");
    auto attributes = Json::object(); attributes["version"] = "fixture-build"; attributes["attachment.log"] = "complete";
    {
        MultipartReport body(dump, attributes, "fixture-boundary");
        check(body.error() == 0, "Cannot prepare attached report");
        std::string wire;
        check(body.write([&](const void* data, DWORD bytes) { wire.append(static_cast<const char*>(data), bytes); return true; }), "Cannot stream attached report");
        check(wire.size() == body.size(), "Multipart Content-Length does not match transmitted bytes");
        // Parse the emitted MIME parts independently to check names, boundaries and raw payloads.
        std::map<std::string, std::pair<std::string, std::string>> parts;
        const std::string delimiter = "--fixture-boundary";
        std::size_t cursor{};
        for (;;) {
            check(wire.compare(cursor, delimiter.size(), delimiter) == 0, "Malformed MIME delimiter");
            cursor += delimiter.size();
            if (wire.compare(cursor, 4, "--\r\n") == 0) { check(cursor + 4 == wire.size(), "Trailing MIME data"); break; }
            check(wire.compare(cursor, 2, "\r\n") == 0, "Missing MIME header separator"); cursor += 2;
            const auto end = wire.find("\r\n\r\n", cursor), next = wire.find("\r\n" + delimiter, end + 4);
            check(end != std::string::npos && next != std::string::npos, "Unterminated MIME part");
            const auto header = wire.substr(cursor, end - cursor);
            const auto name = header.find("name=\"");
            check(name != std::string::npos, "Missing MIME field name");
            const auto name_end = header.find('"', name + 6);
            check(name_end != std::string::npos, "Invalid MIME field name");
            check(parts.emplace(header.substr(name + 6, name_end - name - 6),
                std::make_pair(header, wire.substr(end + 4, next - end - 4))).second, "Duplicate MIME field");
            cursor = next + 2;
        }
        check(parts.size() == 5 && parts.at("version").second == "fixture-build", "Attributes lost during multipart upload");
        check(parts.at("upload_file_minidump").second == dump_bytes, "Dump bytes corrupted");
        check(parts.at("attachment_ReSkate.log").second == original_log &&
            parts.at("attachment_ReSkate.log").first.find("filename=\"ReSkate.log\"") != std::string::npos, "Log is not a named file attachment");
        check(Json::parse(parts.at("attachment_report.json").second).at("version").string() == "fixture-build", "Report details attachment missing");
    }
    {
        MultipartReport body(dump, attributes, "fixture-boundary");
        check(!body.write([](const void*, DWORD) { return false; }), "Stream failure ignored");
    }
    std::filesystem::remove(log_attachment_path(dump));
    check(MultipartReport(dump, attributes, "fixture-boundary").error() != 0, "Missing queued log silently dropped");
    attributes.erase("attachment.log");
    check(MultipartReport(dump, attributes, "fixture-boundary").error() == 0, "Legacy report without logs blocked");
    const std::string tail(max_log_bytes, 'x');
    std::ofstream(log, std::ios::binary) << "discard this old prefix" << tail;
    check(snapshot_log(log, dump) == LogSnapshot::tail && read(log_attachment_path(dump)) == tail, "Large log did not retain bounded tail");
    check(std::filesystem::file_size(log) > max_log_bytes, "Original log was truncated");
    check(snapshot_log(directory / L"missing.log", directory / L"missing.dmp") == LogSnapshot::unavailable, "Missing source log reported as attached");
}
void queue_checks(const std::filesystem::path& root) {
    const auto queue = root / L"queue";
    std::filesystem::create_directories(queue);
    const auto dump = queue / L"fixture.dmp";
    const auto metadata = dump.wstring() + L".json";
    std::ofstream(dump) << "synthetic test data";
    std::ofstream(metadata) << R"({"version":"original-build","attachment.log":"complete"})";
    std::ofstream(log_attachment_path(dump)) << "original-session-log";
    const Sender failure = [](const auto&, const auto&, const auto&) { return UploadResult{false, 503, 0, {}}; };
    check(upload_pending(L"test", queue, failure) == 0 && std::filesystem::exists(dump), "Failed upload did not retain dump");
    check(read(log_attachment_path(dump)) == "original-session-log", "Failed upload lost log attachment");
    const Sender success = [](const auto&, const auto& queued_dump, const Json& attributes) {
        check(attributes.at("version").string() == "original-build", "Queued report lost original build attributes");
        check(read(log_attachment_path(queued_dump)) == "original-session-log", "Retry lost original session log");
        return UploadResult{true, 200, 0, "1234-abcd"};
    };
    Handle lock(CreateFileW((queue / L"queue.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(upload_pending(L"test", queue, success) == 0 && std::filesystem::exists(dump), "Queue lock failed");
    CloseHandle(lock.release());
    check(upload_pending(L"test", queue, success) == 1, "Confirmed upload not counted");
    check(!std::filesystem::exists(dump) && !std::filesystem::exists(metadata) && !std::filesystem::exists(log_attachment_path(dump)), "Confirmed report or attachment not removed");
    check(queue_directory(root, L"https://a/token/minidump") != queue_directory(root, L"https://b/token/minidump"), "Different destinations share queue");
    for (std::size_t i = 0; i < max_pending_reports + 2; ++i) {
        const auto path = queue / std::format(L"{}.dmp", i);
        std::ofstream(path) << "synthetic test data";
        std::ofstream(path.wstring() + L".json") << R"({"version":"original-build"})";
        std::ofstream(log_attachment_path(path)) << "retention-fixture";
    }
    upload_pending(L"test", queue, failure);
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(queue)) {
        if (entry.path().extension() == L".dmp") ++count;
        if (entry.path().extension() == L".log") {
            auto owner = entry.path(); owner.replace_extension();
            check(std::filesystem::exists(owner), "Retention orphaned a log attachment");
        }
    }
    check(count == max_pending_reports, "Queue retention limit not enforced");
    const auto expired = queue / L"expired.dmp";
    std::ofstream(expired) << "expired";
    std::ofstream(expired.wstring() + L".json") << "{}";
    std::ofstream(log_attachment_path(expired)) << "expired-session-log";
    std::filesystem::last_write_time(expired, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 15));
    upload_pending(L"test", queue, failure);
    check(!std::filesystem::exists(expired) && !std::filesystem::exists(log_attachment_path(expired)), "Expired report or attachment retained");
}
void crash_check(const std::filesystem::path& root, bool native) {
    std::filesystem::create_directories(root);
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    std::filesystem::path child_path(executable.data());
    if (native) {
        child_path = root / L"Skate.exe";
        std::filesystem::copy_file(executable.data(), child_path);
        std::filesystem::copy_file(std::filesystem::path(executable.data()).parent_path() / L"ReSkateLauncher.exe", root / L"ReSkateLauncher.exe");
    }
    auto command = std::format(L"\"{}\" {} \"{}\"", child_path.wstring(), native ? L"--native-crash" : L"--crash", root.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    check(CreateProcessW(child_path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "Cannot start crash fixture");
    Handle child(process.hProcess), thread(process.hThread);
    check(WaitForSingleObject(child.value, 30000) == WAIT_OBJECT_0, "Crash fixture timed out");
    DWORD code{}; GetExitCodeProcess(child.value, &code);
    check(code == 42, "Previous exception handler was not preserved");
    const auto log = read(root / L"ReSkate.log");
    check(log.find("Backtrace crash reporting enabled") != std::string::npos, "Helper did not start");
    check((log.find("ReSkate unhandled exception") != std::string::npos) != native, "Fixture did not exercise the expected exception handler");
    const auto queue = queue_directory(root / L"crashes", L"https://127.0.0.1:1/test/minidump");
    std::filesystem::path dump;
    for (int attempt = 0; attempt < 100 && dump.empty(); ++attempt) {
        for (const auto& entry : std::filesystem::directory_iterator(queue)) if (entry.path().extension() == L".dmp") dump = entry.path();
        if (dump.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(!dump.empty(), "No out-of-process minidump captured");
    auto bytes = read(dump);
    MINIDUMP_DIRECTORY* stream{}; void* data{}; ULONG size{};
    check(MiniDumpReadDumpStream(bytes.data(), ExceptionStream, &stream, &data, &size) != FALSE, "Minidump exception stream missing");
    const auto* exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(data);
    check(size >= sizeof(*exception) && exception->ExceptionRecord.ExceptionCode == test_exception, "Wrong exception captured");
    check(MiniDumpReadDumpStream(bytes.data(), ModuleListStream, &stream, &data, &size) != FALSE, "Minidump module list missing");
    check(Json::parse(read(dump.wstring() + L".json")).at("application").string() == "ReSkate", "Crash attributes missing");
    check(Json::parse(read(dump.wstring() + L".json")).at("capture.source").string() ==
        (native ? "game-native" : "unhandled-exception"), "Wrong capture source");
    check(Json::parse(read(dump.wstring() + L".json")).at("attachment.log").string() == "complete" &&
        read(log_attachment_path(dump)) == log, "Crash did not snapshot its session log");
    if (native) {
        const auto source = root / L"local" / L"ReSkate" / L"Game" / L"Skate" / L"CrashDumps";
        const auto pid = GetProcessId(child.value), created = process_creation_seconds(child.value);
        const auto selected = find_native_dump(source, pid, created);
        check(selected && selected->path.extension() == L".mdmp", "Native dump selection failed");
        check(!find_native_dump(source, pid + 1, created), "Imported a different process's dump");
        check(!find_native_dump(source, pid, created - 1), "Imported a different lifetime of the same PID");
        check(std::filesystem::exists(source / L"game-crash.mdmp"), "Original game dump was removed");
        std::filesystem::create_directories(root / L"invalid");
        std::ofstream(root / L"invalid" / L"bad.mdmp") << "MDMP";
        check(!find_native_dump(root / L"invalid", pid, created), "Truncated native dump accepted");
    }
    // Allow the helper's deliberately refused loopback upload to finish before fixture cleanup.
    for (int i = 0; i < 100 && !std::filesystem::exists(queue / L"status.txt"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(std::filesystem::exists(dump), "Offline dump was lost");
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--inspect-native") {
            const auto dump = find_native_dump(argv[2], static_cast<DWORD>(std::stoul(argv[3])), static_cast<DWORD>(std::stoul(argv[4])));
            check(dump.has_value(), "No native dump matched this process lifetime");
            std::cout << "Matched native crash: process " << dump->process_id << "; exception 0x" << std::hex
                << dump->exception_code << "; address 0x" << dump->exception_address << '\n';
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--submit-fixture") {
            const std::filesystem::path dump(argv[2]);
            auto bytes = read(dump);
            check(bytes.size() >= sizeof(MINIDUMP_HEADER), "Not a minidump fixture");
            MINIDUMP_DIRECTORY* stream{}; void* data{}; ULONG size{};
            check(MiniDumpReadDumpStream(bytes.data(), ExceptionStream, &stream, &data, &size) != FALSE &&
                size >= sizeof(MINIDUMP_EXCEPTION_STREAM) &&
                static_cast<const MINIDUMP_EXCEPTION_STREAM*>(data)->ExceptionRecord.ExceptionCode == test_exception,
                "Only synthetic regression-test crashes may be submitted by this command");
            auto attributes = Json::parse(read(dump.wstring() + L".json"));
            attributes["integration.test"] = "true";
            attributes["error.message"] = "Synthetic ReSkate Backtrace integration test; no game crash";
            auto url = environment(L"RESKATE_BACKTRACE_URL");
            if (url.empty()) url = configured_url;
            const auto result = upload_report(url, dump, attributes);
            std::cout << "HTTP " << result.http_status << "; Windows error " << result.error << "; report " << result.report_id << '\n';
            check(result.accepted, "Backtrace did not confirm test report");
            return 0;
        }
        if (argc == 3 && (std::wstring_view(argv[1]) == L"--crash" || std::wstring_view(argv[1]) == L"--native-crash")) {
            const bool native = std::wstring_view(argv[1]) == L"--native-crash";
            if (native) {
                const auto local = std::filesystem::path(argv[2]) / L"local";
                SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
                native_fixture_directory = local / L"ReSkate" / L"Game" / L"Skate" / L"CrashDumps";
                std::filesystem::create_directories(native_fixture_directory);
            }
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"1");
            SetEnvironmentVariableW(L"RESKATE_BACKTRACE_URL", L"https://127.0.0.1:1/test/minidump");
            SetUnhandledExceptionFilter(finish_child);
            logging::Options options; options.directory = argv[2]; options.truncate_file = true;
            check(logging::initialize(options), "Fixture logging failed");
            if (native) SetUnhandledExceptionFilter(native_child_handler); // Reproduce the game's later handler registration.
            RaiseException(test_exception, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 3;
        }
        check(argc == 2, "Pass a fixture root");
        const auto root = std::filesystem::path(argv[1]) / std::format(L"run-{}-{}", GetCurrentProcessId(), GetTickCount64());
        std::filesystem::create_directories(root);
        SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"0");
        check(start(root) == StartResult::disabled, "Explicit opt-out ignored");
        SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"1");
        SetEnvironmentVariableW(L"RESKATE_BACKTRACE_URL", L"http://invalid/post");
        check(start(root) == StartResult::failed, "Invalid endpoint enabled reporter");
        endpoint_checks();
        attachment_checks(root);
        queue_checks(root);
        crash_check(root / L"direct", false);
        crash_check(root / L"native", true);
        std::cout << "Backtrace endpoint, receipts, multipart attachments, log snapshots, retries, retention, opt-out, exception chaining, direct capture and native-handler fallback checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
