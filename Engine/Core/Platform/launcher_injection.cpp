#include "launcher_support.h"
#include "launcher_support_internal.h"
#include "path_text.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace dingosdk::launcher {
namespace {
using detail::fail;
using detail::fail_windows;
using detail::Handle;

// What is loaded into the child that Windows did not put there. One of these
// is usually what hooked LoadLibraryW, and naming it beats telling someone to
// go looking for "security software".
std::string foreign_modules(HANDLE process) {
    std::array<wchar_t, MAX_PATH> windows{};
    const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    const std::wstring system_root(windows.data(), length && length < windows.size() ? length : 0);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(process)));
    if (snapshot.get() == INVALID_HANDLE_VALUE || !snapshot.get()) return {};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring game_root;
    std::string names;
    std::size_t shown = 0;
    for (auto more = Module32FirstW(snapshot.get(), &entry); more;
         more = Module32NextW(snapshot.get(), &entry)) {
        const std::wstring path(entry.szExePath);
        // The first module is Skate.exe itself; everything beside it is the
        // install, which is not what we are looking for.
        if (game_root.empty()) {
            game_root = fs::path(path).parent_path().wstring();
            continue;
        }
        const auto under = [&path](const std::wstring& root) {
            return !root.empty() && path.size() > root.size() &&
                   _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0;
        };
        if (under(system_root) || under(game_root)) continue;
        if (shown == 8) { names += ", and more"; break; }
        names += (names.empty() ? "" : ", ") + path_utf8(fs::path(entry.szModule));
        ++shown;
    }
    return names;
}

// The length of a jump written over a function's first bytes, or zero when the
// bytes are not one of the forms a hooking engine writes. Anti-virus and
// overlays hook by patching a jump to their own code at the entry and running
// the original afterwards; what they do not do is replace the whole function.
std::size_t inline_jump_length(const unsigned char* bytes, std::size_t size) {
    const auto at = [&](std::size_t index) { return index < size ? bytes[index] : 0u; };
    if (size >= 5 && (at(0) == 0xE9 || at(0) == 0xE8)) return 5;              // jmp/call rel32
    if (size >= 6 && at(0) == 0x68 && at(5) == 0xC3) return 6;                // push imm32; ret
    if (size >= 6 && at(0) == 0xFF && at(1) == 0x25) {
        // jmp [rip+rel32]. The common form targets the eight bytes that follow.
        const bool next = at(2) == 0 && at(3) == 0 && at(4) == 0 && at(5) == 0;
        return next && size >= 14 ? 14 : 6;
    }
    if (size >= 12 && at(0) == 0x48 && at(1) == 0xB8 && at(10) == 0xFF && at(11) == 0xE0)
        return 12;                                                            // mov rax, imm64; jmp rax
    if (size >= 2 && at(0) == 0xEB) return 2;                                 // short jmp, as hotpatching writes
    return 0;
}

bool executable(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xff) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

class RemoteEntrypointPatch {
public:
    RemoteEntrypointPatch(HANDLE process, std::uintptr_t address)
        : process_(process), address_(reinterpret_cast<void*>(address)) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQueryEx(process_, address_, &memory, sizeof(memory)))
            fail_windows("Cannot inspect the executable entrypoint");
        const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
            memory.RegionSize;
        if (memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            !executable(memory.Protect) || address > region_end ||
            original_.size() > region_end - address)
            fail("Executable entrypoint is not in a writable image range");
        SIZE_T count{};
        if (!ReadProcessMemory(process_, address_, original_.data(), original_.size(), &count) ||
            count != original_.size())
            fail_windows("Cannot read the executable entrypoint");
        constexpr std::array<unsigned char, 2> spin{0xeb, 0xfe};
        if (original_ == spin) fail("Executable entrypoint is already patched");
        DWORD protection{};
        if (!VirtualProtectEx(process_, address_, spin.size(), PAGE_EXECUTE_READWRITE, &protection))
            fail_windows("Cannot make the executable entrypoint writable");
        original_protection_ = protection;
        const auto wrote = WriteProcessMemory(process_, address_, spin.data(), spin.size(), &count) &&
            count == spin.size();
        if (wrote) active_ = true;
        DWORD ignored{};
        const auto protected_again = VirtualProtectEx(
            process_, address_, spin.size(), original_protection_, &ignored) != FALSE;
        if (!wrote) fail_windows("Cannot install the executable entrypoint gate");
        if (!FlushInstructionCache(process_, address_, spin.size()))
            fail_windows("Cannot flush the executable entrypoint gate");
        if (!protected_again)
            fail_windows("Cannot restore executable entrypoint protection");
    }

    ~RemoteEntrypointPatch() { restore_noexcept(); }
    RemoteEntrypointPatch(const RemoteEntrypointPatch&) = delete;
    RemoteEntrypointPatch& operator=(const RemoteEntrypointPatch&) = delete;

    void restore() {
        if (!active_) return;
        DWORD protection{};
        if (!VirtualProtectEx(process_, address_, original_.size(), PAGE_EXECUTE_READWRITE,
                &protection))
            fail_windows("Cannot make the gated entrypoint writable for restoration");
        SIZE_T count{};
        const auto wrote = WriteProcessMemory(process_, address_, original_.data(),
            original_.size(), &count) && count == original_.size();
        const auto flushed = wrote && FlushInstructionCache(
            process_, address_, original_.size()) != FALSE;
        DWORD ignored{};
        const auto protected_again = VirtualProtectEx(process_, address_, original_.size(),
            original_protection_, &ignored) != FALSE;
        if (!wrote) fail_windows("Cannot restore the executable entrypoint");
        if (!flushed) fail_windows("Cannot flush the restored executable entrypoint");
        if (!protected_again) fail_windows("Cannot restore executable entrypoint protection");
        active_ = false;
    }

private:
    void restore_noexcept() noexcept {
        if (!active_) return;
        DWORD protection{};
        if (!VirtualProtectEx(process_, address_, original_.size(), PAGE_EXECUTE_READWRITE,
                &protection))
            return;
        SIZE_T count{};
        if (WriteProcessMemory(process_, address_, original_.data(), original_.size(), &count) &&
            count == original_.size())
            FlushInstructionCache(process_, address_, original_.size());
        DWORD ignored{};
        VirtualProtectEx(process_, address_, original_.size(), original_protection_, &ignored);
        active_ = false;
    }

    HANDLE process_{};
    void* address_{};
    std::array<unsigned char, 2> original_{};
    DWORD original_protection_{};
    bool active_{};
};

// `size` bytes at `rva` of a PE image as the loader maps it at `base`, read from
// the file on disk and relocated. Overlays and security tools hook LoadLibraryW
// in running processes, so the file is the reference that no one has patched.
std::vector<unsigned char> image_file_bytes(const fs::path& file, std::uint32_t rva, std::size_t size, std::uintptr_t base) {
    std::vector<unsigned char> image;
    {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER length{};
        if (GetFileSizeEx(handle, &length) && length.QuadPart > 0 && length.QuadPart < 64ll * 1024 * 1024) {
            image.resize(static_cast<std::size_t>(length.QuadPart));
            DWORD read{};
            if (!ReadFile(handle, image.data(), static_cast<DWORD>(image.size()), &read, nullptr) || read != image.size())
                image.clear();
        }
        CloseHandle(handle);
    }
    const auto in_file = [&](std::size_t offset, std::size_t count) { return offset <= image.size() && count <= image.size() - offset; };
    if (!in_file(0, sizeof(IMAGE_DOS_HEADER))) return {};
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || !in_file(static_cast<std::size_t>(dos.e_lfanew), sizeof(IMAGE_NT_HEADERS64)))
        return {};
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    const auto sections_at = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    const auto file_offset = [&](std::uint32_t address, std::size_t count) -> std::optional<std::size_t> {
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto at = sections_at + i * sizeof(IMAGE_SECTION_HEADER);
            if (!in_file(at, sizeof(IMAGE_SECTION_HEADER))) return std::nullopt;
            IMAGE_SECTION_HEADER section{};
            std::memcpy(&section, image.data() + at, sizeof(section));
            if (address >= section.VirtualAddress && address - section.VirtualAddress + count <= section.SizeOfRawData) {
                const auto offset = static_cast<std::size_t>(section.PointerToRawData) + (address - section.VirtualAddress);
                return in_file(offset, count) ? std::optional(offset) : std::nullopt;
            }
        }
        return std::nullopt;
    };
    // Read with 8 bytes either side so a relocation straddling an edge is still applied whole.
    constexpr std::uint32_t pad = 8;
    if (rva < pad) return {};
    const auto window = file_offset(rva - pad, size + pad * 2);
    if (!window) return {};
    std::vector<unsigned char> bytes(image.begin() + static_cast<std::ptrdiff_t>(*window),
                                     image.begin() + static_cast<std::ptrdiff_t>(*window + size + pad * 2));
    const auto delta = base - static_cast<std::uintptr_t>(nt.OptionalHeader.ImageBase);
    const auto& relocations = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && relocations.Size) {
        const auto table = file_offset(relocations.VirtualAddress, relocations.Size);
        if (!table) return {};
        for (std::size_t at = 0; at + sizeof(IMAGE_BASE_RELOCATION) <= relocations.Size;) {
            IMAGE_BASE_RELOCATION block{};
            std::memcpy(&block, image.data() + *table + at, sizeof(block));
            if (block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > relocations.Size - at) return {};
            for (std::size_t entry = sizeof(block); entry + 2 <= block.SizeOfBlock; entry += 2) {
                std::uint16_t value{};
                std::memcpy(&value, image.data() + *table + at + entry, sizeof(value));
                if ((value >> 12) != IMAGE_REL_BASED_DIR64) continue;
                const std::uint64_t target = static_cast<std::uint64_t>(block.VirtualAddress) + (value & 0xfff);
                if (target < rva - pad || target + 8 > static_cast<std::uint64_t>(rva) + size + pad) continue;
                std::uint64_t pointer{};
                std::memcpy(&pointer, bytes.data() + (target - (rva - pad)), sizeof(pointer));
                pointer += delta;
                std::memcpy(bytes.data() + (target - (rva - pad)), &pointer, sizeof(pointer));
            }
            at += block.SizeOfBlock;
        }
    }
    return std::vector<unsigned char>(bytes.begin() + pad, bytes.begin() + pad + static_cast<std::ptrdiff_t>(size));
}
} // namespace

LoaderGate::~LoaderGate() {
    for (const auto& thread : threads_) if (thread.handle) CloseHandle(thread.handle);
}

LoaderGate::LoaderGate(LoaderGate&& other) noexcept
    : threads_(std::move(other.threads_)), entrypoint_(other.entrypoint_),
      exiting_threads_(other.exiting_threads_), resumed_(other.resumed_) {
    other.threads_.clear();
    other.resumed_ = true;
}

LoaderGate& LoaderGate::operator=(LoaderGate&& other) noexcept {
    if (this == &other) return *this;
    for (const auto& thread : threads_) if (thread.handle) CloseHandle(thread.handle);
    threads_ = std::move(other.threads_);
    entrypoint_ = other.entrypoint_;
    exiting_threads_ = other.exiting_threads_;
    resumed_ = other.resumed_;
    other.threads_.clear();
    other.resumed_ = true;
    return *this;
}

void LoaderGate::resume() {
    if (resumed_) fail("Loader-gated threads were already resumed");
    const auto resume_group = [this](bool primary) {
        for (const auto& thread : threads_) {
            if (thread.primary != primary) continue;
            const auto previous = ResumeThread(thread.handle);
            if (previous == static_cast<DWORD>(-1) && !thread.primary &&
                WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0)
                continue;
            if (previous == static_cast<DWORD>(-1))
                fail_windows("Cannot restore a loader-gated thread");
            if (previous != thread.previous_suspend_count + 1)
                fail("A loader-gated thread has an unexpected suspend count");
        }
    };
    // Keep the executable entrypoint gated until all pre-existing worker
    // threads have had their original suspension counts restored.
    resume_group(false);
    resume_group(true);
    resumed_ = true;
}

LoaderGate prepare_loader_for_injection(HANDLE process, HANDLE primary_thread,
                                        DWORD process_id, std::uintptr_t image_base) {
    if (!process || !primary_thread || !image_base || GetProcessId(process) != process_id)
        fail("Invalid suspended process handles");
    const auto primary_id = GetThreadId(primary_thread);
    if (!primary_id) fail_windows("Cannot identify the suspended primary thread");

    IMAGE_DOS_HEADER dos{};
    SIZE_T count{};
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(image_base), &dos,
            sizeof(dos), &count) || count != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
        fail("Cannot validate the gated executable DOS header");
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(process,
            reinterpret_cast<const void*>(image_base + static_cast<std::uintptr_t>(dos.e_lfanew)),
            &nt, sizeof(nt), &count) || count != sizeof(nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !nt.OptionalHeader.AddressOfEntryPoint ||
        nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage)
        fail("Cannot validate the gated executable PE header");
    const auto entrypoint = image_base + nt.OptionalHeader.AddressOfEntryPoint;
    if (entrypoint < image_base) fail("Executable entrypoint address overflowed");
    RemoteEntrypointPatch patch(process, entrypoint);

    bool primary_running = false;
    try {
        const auto initial_suspend_count = ResumeThread(primary_thread);
        if (initial_suspend_count == static_cast<DWORD>(-1))
            fail_windows("Cannot begin Windows loader initialization");
        if (initial_suspend_count != 1) {
            SuspendThread(primary_thread);
            fail("Primary thread did not have the expected creation suspension");
        }
        primary_running = true;

        LoaderGate gate;
        gate.entrypoint_ = entrypoint;
        const auto deadline = GetTickCount64() + 30000;
        for (;;) {
            if (GetTickCount64() >= deadline)
                fail("Windows loader initialization timed out before the executable entrypoint");
            const auto previous = SuspendThread(primary_thread);
            if (previous == static_cast<DWORD>(-1))
                fail_windows("Cannot sample the executable primary thread");
            primary_running = false;
            if (previous != 0)
                fail("Executable primary thread acquired an unexpected suspension");
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(primary_thread, &context))
                fail_windows("Cannot read the executable primary-thread context");
            if (context.Rip == entrypoint) {
                HANDLE owned_primary{};
                if (!DuplicateHandle(GetCurrentProcess(), primary_thread, GetCurrentProcess(),
                        &owned_primary, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    fail_windows("Cannot retain the gated primary thread");
                gate.threads_.push_back({owned_primary, primary_id, previous, true});
                break;
            }
            const auto resumed = ResumeThread(primary_thread);
            if (resumed == static_cast<DWORD>(-1))
                fail_windows("Cannot continue Windows loader initialization");
            if (resumed != 1)
                fail("Executable primary thread changed suspension while sampled");
            primary_running = true;
            Sleep(1);
        }

        // Once the primary thread is held in the entrypoint loop, iteratively
        // capture loader-created workers. Repeating until a stable snapshot
        // closes the creation race: after every observed thread is suspended,
        // no target thread remains able to create another one.
        const auto suspend_thread = reinterpret_cast<LONG(NTAPI*)(HANDLE, PULONG)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendThread"));
        if (!suspend_thread) fail_windows("Cannot locate NtSuspendThread");
        constexpr LONG status_invalid_handle = static_cast<LONG>(0xC0000008);
        constexpr LONG status_thread_is_terminating = static_cast<LONG>(0xC000004B);
        std::vector<DWORD> exiting;
        bool stable = false;
        for (unsigned pass = 0; pass < 16 && !stable; ++pass) {
            stable = true;
            Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            if (snapshot.get() == INVALID_HANDLE_VALUE)
                fail_windows("Snapshot loader-initialized threads");
            THREADENTRY32 thread{sizeof(thread)};
            if (!Thread32First(snapshot.get(), &thread))
                fail_windows("Enumerate loader-initialized threads");
            do {
                if (thread.th32OwnerProcessID != process_id || thread.th32ThreadID == primary_id) {
                    thread.dwSize = sizeof(thread);
                    continue;
                }
                const auto known = std::any_of(gate.threads_.begin(), gate.threads_.end(),
                    [&thread](const LoaderGate::Thread& captured) {
                        return captured.id == thread.th32ThreadID;
                    });
                if (known) {
                    thread.dwSize = sizeof(thread);
                    continue;
                }
                const auto handle = OpenThread(THREAD_SUSPEND_RESUME | SYNCHRONIZE |
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread.th32ThreadID);
                if (!handle) {
                    if (GetLastError() == ERROR_INVALID_PARAMETER) {
                        thread.dwSize = sizeof(thread);
                        continue;
                    }
                    fail_windows("Open loader-initialized thread");
                }
                ULONG previous{};
                const auto status = suspend_thread(handle, &previous);
                if (status < 0) {
                    CloseHandle(handle);
                    // A thread that is already exiting (loader-time helper threads of
                    // injected mods such as OptiScaler) can never run code again, so it
                    // needs no gate. SuspendThread reports it only as access denied.
                    if (status == status_thread_is_terminating) {
                        if (std::find(exiting.begin(), exiting.end(), thread.th32ThreadID) == exiting.end())
                            exiting.push_back(thread.th32ThreadID);
                        thread.dwSize = sizeof(thread);
                        continue;
                    }
                    if (status == status_invalid_handle) {
                        thread.dwSize = sizeof(thread);
                        continue;
                    }
                    std::ostringstream detail;
                    detail << "Suspend loader-initialized thread " << thread.th32ThreadID
                           << " failed (NTSTATUS 0x" << std::hex << static_cast<ULONG>(status) << ')';
                    throw std::runtime_error(detail.str());
                }
                gate.threads_.push_back({handle, thread.th32ThreadID, previous, false});
                stable = false;
                thread.dwSize = sizeof(thread);
            } while (Thread32Next(snapshot.get(), &thread));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                fail_windows("Enumerate loader-initialized threads");
        }
        if (!stable) fail("Loader-created threads did not reach a stable gated set");
        gate.exiting_threads_ = exiting.size();

        patch.restore();
        return gate;
    } catch (...) {
        if (primary_running) SuspendThread(primary_thread);
        throw;
    }
}

std::uintptr_t validated_remote_load_library(HANDLE process, std::string* note) {
    const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) fail_windows("Cannot locate local kernel32.dll");
    const auto symbol = GetProcAddress(kernel32, "LoadLibraryW");
    if (!symbol) fail_windows("Cannot locate local LoadLibraryW");
    const auto address = reinterpret_cast<std::uintptr_t>(symbol);

    MEMORY_BASIC_INFORMATION local{}, remote{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &local, sizeof(local)))
        fail_windows("Cannot inspect local LoadLibraryW");
    if (!VirtualQueryEx(process, reinterpret_cast<const void*>(address), &remote, sizeof(remote)))
        fail_windows("Cannot inspect child LoadLibraryW");
    if (local.State != MEM_COMMIT || remote.State != MEM_COMMIT ||
        local.Type != MEM_IMAGE || remote.Type != MEM_IMAGE ||
        !executable(local.Protect) || !executable(remote.Protect) ||
        local.AllocationBase != remote.AllocationBase) {
        std::ostringstream detail;
        detail << "The child does not share the validated LoadLibraryW KnownDLL mapping"
               << " (local base=" << local.AllocationBase << " state=" << std::hex << local.State
               << " type=" << local.Type << " protect=" << local.Protect
               << ", remote base=" << remote.AllocationBase << " state=" << remote.State
               << " type=" << remote.Type << " protect=" << remote.Protect << ')';
        throw std::runtime_error(detail.str());
    }

    wchar_t provider_path[32768]{};
    const auto provider_length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(local.AllocationBase), provider_path, 32768);
    if (!provider_length || provider_length >= 32768)
        fail_windows("Cannot identify the LoadLibraryW provider");
    const auto provider_name = fs::path(std::wstring(provider_path, provider_length)).filename();
    if (_wcsicmp(provider_name.c_str(), L"kernel32.dll") != 0 &&
        _wcsicmp(provider_name.c_str(), L"kernelbase.dll") != 0)
        fail("LoadLibraryW is not owned by an expected Windows KnownDLL");

    const auto local_base = reinterpret_cast<std::uintptr_t>(local.AllocationBase);
    const auto remote_base = reinterpret_cast<std::uintptr_t>(remote.AllocationBase);
    const auto dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(local_base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
        fail("Local LoadLibraryW provider has an invalid image header");
    const auto local_nt = *reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        local_base + static_cast<std::uintptr_t>(dos.e_lfanew));
    IMAGE_DOS_HEADER remote_dos{};
    SIZE_T read{};
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(remote_base), &remote_dos,
            sizeof(remote_dos), &read) || read != sizeof(remote_dos) ||
        remote_dos.e_magic != dos.e_magic || remote_dos.e_lfanew != dos.e_lfanew)
        fail_windows("Cannot validate the child LoadLibraryW provider header");
    IMAGE_NT_HEADERS64 remote_nt{};
    if (!ReadProcessMemory(process,
            reinterpret_cast<const void*>(remote_base + static_cast<std::uintptr_t>(remote_dos.e_lfanew)),
            &remote_nt, sizeof(remote_nt), &read) || read != sizeof(remote_nt) ||
        remote_nt.Signature != IMAGE_NT_SIGNATURE ||
        remote_nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        remote_nt.FileHeader.TimeDateStamp != local_nt.FileHeader.TimeDateStamp ||
        remote_nt.OptionalHeader.SizeOfImage != local_nt.OptionalHeader.SizeOfImage ||
        remote_nt.OptionalHeader.CheckSum != local_nt.OptionalHeader.CheckSum)
        fail("The child LoadLibraryW provider image does not match this process");
    if (address < remote_base || address - remote_base >= remote_nt.OptionalHeader.SizeOfImage)
        fail("Child LoadLibraryW is outside its provider image");

    constexpr std::size_t fingerprint_size = 32;
    const auto local_region_end = reinterpret_cast<std::uintptr_t>(local.BaseAddress) + local.RegionSize;
    const auto remote_region_end = reinterpret_cast<std::uintptr_t>(remote.BaseAddress) + remote.RegionSize;
    if (address > local_region_end || fingerprint_size > local_region_end - address ||
        address > remote_region_end || fingerprint_size > remote_region_end - address)
        fail("LoadLibraryW entry bytes cross an unexpected memory region");
    std::array<unsigned char, fingerprint_size> local_bytes{}, actual{};
    std::memcpy(local_bytes.data(), reinterpret_cast<const void*>(address), local_bytes.size());
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), actual.data(),
            actual.size(), &read) || read != actual.size())
        fail_windows("Cannot read the child LoadLibraryW entry");
    // The reference is the provider file on disk: overlays (MSI Afterburner/RTSS,
    // Discord, OBS, Overwolf) and security tools often hook LoadLibraryW in the
    // launcher's own window process, while the suspended child is still pristine.
    const auto clean = image_file_bytes(fs::path(std::wstring(provider_path, provider_length)),
        static_cast<std::uint32_t>(address - local_base), fingerprint_size, remote_base);
    const bool matches_file = clean.size() == actual.size() && std::equal(clean.begin(), clean.end(), actual.begin());
    // A hook that writes a jump to a per-process trampoline leaves different
    // bytes in the launcher and in the child, so neither comparison above can
    // match even though nothing is wrong. Accept that shape when the jump is
    // the only difference: everything past it is still Windows' own code, and
    // the provider image already matched by timestamp, checksum and size.
    bool hooked_entry = false;
    if (!matches_file && actual != local_bytes && clean.size() == actual.size()) {
        const auto jump = inline_jump_length(actual.data(), actual.size());
        hooked_entry = jump > 0 && jump < actual.size() &&
            std::equal(clean.begin() + static_cast<std::ptrdiff_t>(jump), clean.end(),
                       actual.begin() + static_cast<std::ptrdiff_t>(jump));
    }
    if (!matches_file && actual != local_bytes && !hooked_entry) {
        const auto foreign = foreign_modules(process);
        std::string message = "Skate's LoadLibraryW has been replaced, not just hooked: its entry bytes match "
                              "neither Windows' kernel32 file nor this process, and what follows them is not "
                              "Windows' code either.";
        if (!foreign.empty()) message += " Loaded into Skate and not part of Windows: " + foreign + ".";
        fail(message.c_str());
    }
    if (hooked_entry && note) {
        *note = "Skate's LoadLibraryW is hooked at its entry; the rest of it is Windows' own, so ReSkate is "
                "loading through it.";
        if (const auto foreign = foreign_modules(process); !foreign.empty())
            *note += " Loaded into Skate and not part of Windows: " + foreign + ".";
    }
    return address;
}

} // namespace dingosdk::launcher
