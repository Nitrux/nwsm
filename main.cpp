// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <utility>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <syslog.h>
#include <system_error>
#include <sys/socket.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t session_process_pid = -1;
volatile std::sig_atomic_t session_process_pgid = -1;

constexpr std::chrono::milliseconds poll_interval{100};
constexpr std::chrono::seconds default_ready_timeout{60};
constexpr std::chrono::seconds default_finalize_grace{1};
constexpr const char* finalization_file_name = "nwsm.finalize";

struct SocketIdentity {
    dev_t device{};
    ino_t inode{};

    bool operator<(const SocketIdentity& other) const
    {
        if (device != other.device)
            return device < other.device;
        return inode < other.inode;
    }

    bool operator==(const SocketIdentity& other) const
    {
        return device == other.device && inode == other.inode;
    }
};

struct ChildState {
    pid_t pid{-1};
    bool reaped{false};
    int status{0};
};

struct CompositorReadiness {
    std::pair<std::string, SocketIdentity> wayland;
    std::optional<std::string> hyprland_signature;
};

using EnvironmentSnapshot = std::vector<std::pair<std::string, std::string>>;

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1)
        : m_fd(fd)
    {
    }

    ~ScopedFd()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept
    {
        if (this != &other) {
            if (m_fd >= 0)
                ::close(m_fd);
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const
    {
        return m_fd;
    }

    [[nodiscard]] bool valid() const
    {
        return m_fd >= 0;
    }

private:
    int m_fd{-1};
};

struct RuntimeDirectory {
    ScopedFd descriptor;
    fs::path path;
};

void signal_handler(int)
{
    stop_requested = 1;
    if (session_process_pgid > 0)
        ::kill(-static_cast<pid_t>(session_process_pgid), SIGTERM);
    else if (session_process_pid > 0)
        ::kill(static_cast<pid_t>(session_process_pid), SIGTERM);
}

void install_signal_handlers()
{
    struct sigaction action {};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
}

void log_message(const std::string& message)
{
    ::openlog("nwsm", LOG_PID | LOG_NDELAY, LOG_USER);
    ::syslog(LOG_ERR, "%s", message.c_str());
    ::closelog();
    std::cerr << "nwsm: " << message << '\n';
}

std::optional<std::string> environment_value(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
}

std::optional<RuntimeDirectory> open_runtime_directory(const std::string& runtime)
{
    if (runtime.empty() || runtime.front() != fs::path::preferred_separator) {
        log_message("XDG_RUNTIME_DIR must be an absolute path");
        return std::nullopt;
    }

    ScopedFd descriptor(::open(runtime.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        log_message("XDG_RUNTIME_DIR does not name a directory: " + runtime);
        return std::nullopt;
    }

    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        log_message("XDG_RUNTIME_DIR does not name a directory: " + runtime);
        return std::nullopt;
    }

    if (status.st_uid != ::geteuid()) {
        log_message("XDG_RUNTIME_DIR is not owned by the current user: " + runtime);
        return std::nullopt;
    }

    if ((status.st_mode & 0077) != 0 || (status.st_mode & 0700) != 0700) {
        log_message("XDG_RUNTIME_DIR must be private and mode 0700: " + runtime);
        return std::nullopt;
    }

    if (::faccessat(descriptor.get(), ".", R_OK | W_OK | X_OK, AT_EACCESS) != 0) {
        log_message("XDG_RUNTIME_DIR is not writable: " + runtime);
        return std::nullopt;
    }

    return RuntimeDirectory{std::move(descriptor), fs::path(runtime)};
}

std::optional<ScopedFd> acquire_instance_lock(const RuntimeDirectory& runtime)
{
    ScopedFd lock(::openat(runtime.descriptor.get(), "nwsm.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!lock.valid()) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            log_message("another nwsm instance already owns the user session");
        else
            log_message("could not open the nwsm instance lock: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }

    struct stat status {};
    if (::fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid()) {
        log_message("the nwsm instance lock is not a regular file owned by the current user");
        return std::nullopt;
    }
    if (::fchmod(lock.get(), 0600) != 0) {
        log_message("could not secure the nwsm instance lock: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            log_message("another nwsm instance already owns the user session");
        else
            log_message("could not lock the nwsm instance lock: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }
    return std::optional<ScopedFd>(std::move(lock));
}

std::string resolve_executable(const std::string& executable)
{
    const auto is_trusted_executable = [](const fs::path& path) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            return false;
        if (status.st_uid != 0 || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0)
            return false;
        return ::access(path.c_str(), X_OK) == 0;
    };

    if (executable.find(fs::path::preferred_separator) != std::string::npos)
        return is_trusted_executable(executable) ? executable : std::string{};

    constexpr const char* trusted_directories[] = {
        "/usr/bin",
        "/usr/sbin",
        "/bin",
        "/sbin",
    };
    for (const char* directory : trusted_directories) {
        const fs::path candidate = fs::path(directory) / executable;
        if (is_trusted_executable(candidate))
            return candidate.string();
    }

    return {};
}

[[noreturn]] void execute(const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    ::execvp(argv.front(), argv.data());
    _exit(127);
}

void close_inherited_file_descriptors()
{
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0)
        return;
    if (errno != ENOSYS && errno != EINVAL)
        _exit(127);
#endif

    long maximum = ::sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576)
        maximum = 1048576;
    for (int fd = STDERR_FILENO + 1; fd < maximum; ++fd)
        ::close(fd);
}

std::optional<pid_t> spawn(const std::vector<std::string>& arguments, bool quiet, bool new_process_group = false)
{
    if (arguments.empty())
        return std::nullopt;

    const pid_t pid = ::fork();
    if (pid < 0) {
        log_message("cannot fork " + arguments.front() + ": " + std::strerror(errno));
        return std::nullopt;
    }

    if (pid == 0) {
        if (new_process_group) {
            const pid_t parent = ::getppid();
            if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::setpgid(0, 0) != 0)
                _exit(127);
            if (::getppid() != parent) {
                ::kill(0, SIGTERM);
                _exit(127);
            }
        }

        if (quiet) {
            const int null_device = ::open("/dev/null", O_WRONLY);
            if (null_device >= 0) {
                ::dup2(null_device, STDOUT_FILENO);
                ::dup2(null_device, STDERR_FILENO);
                if (null_device > STDERR_FILENO)
                    ::close(null_device);
            }
        }
        close_inherited_file_descriptors();
        execute(arguments);
    }

    if (new_process_group) {
        if (::setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH)
            log_message("could not place the session command in its own process group: " + std::string(std::strerror(errno)));
    }

    return pid;
}

int wait_for_process(pid_t pid)
{
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return 127;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

int run_command(const std::vector<std::string>& arguments, bool quiet = false)
{
    const auto pid = spawn(arguments, quiet);
    return pid.has_value() ? wait_for_process(*pid) : 127;
}

bool write_all(int descriptor, const char* data, std::size_t size)
{
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(descriptor, data + written, size - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool poll_session_process(ChildState& child)
{
    if (child.reaped)
        return false;

    const pid_t result = ::waitpid(child.pid, &child.status, WNOHANG);
    if (result == 0)
        return true;
    if (result == child.pid) {
        child.reaped = true;
        session_process_pid = -1;
        return false;
    }
    if (result < 0 && errno == EINTR)
        return true;

    child.reaped = true;
    session_process_pid = -1;
    child.status = 1 << 8;
    return false;
}

bool process_group_exists(pid_t process_group)
{
    if (process_group <= 0)
        return false;
    if (::kill(-process_group, 0) == 0)
        return true;
    return errno == EPERM;
}

void terminate_session_process(ChildState& child)
{
    const pid_t process_group = static_cast<pid_t>(session_process_pgid);
    if (process_group > 0)
        ::kill(-process_group, SIGTERM);
    else if (!child.reaped && child.pid > 0)
        ::kill(child.pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!child.reaped)
            poll_session_process(child);
        const bool group_alive = process_group_exists(process_group);
        if (child.reaped && !group_alive)
            break;
        std::this_thread::sleep_for(poll_interval);
    }

    if (process_group_exists(process_group))
        ::kill(-process_group, SIGKILL);

    if (!child.reaped && child.pid > 0) {
        while (::waitpid(child.pid, &child.status, 0) < 0 && errno == EINTR) {
        }
        child.reaped = true;
    }
    session_process_pid = -1;
    session_process_pgid = -1;
}

std::optional<SocketIdentity> socket_identity(const fs::path& path)
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode))
        return std::nullopt;
    if (status.st_uid != ::geteuid())
        return std::nullopt;
    return SocketIdentity{status.st_dev, status.st_ino};
}

bool wait_for_socket_event(int descriptor, short events)
{
    struct pollfd poll_descriptor {descriptor, events, 0};
    while (::poll(&poll_descriptor, 1, 250) < 0) {
        if (errno != EINTR)
            return false;
    }
    return (poll_descriptor.revents & events) != 0;
}

bool write_socket_data(int descriptor, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::send(descriptor, bytes + written, size - written, MSG_NOSIGNAL);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for_socket_event(descriptor, POLLOUT))
            continue;
        return false;
    }
    return true;
}

bool read_socket_data(int descriptor, void* data, std::size_t size)
{
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t received = 0;
    while (received < size) {
        const ssize_t result = ::recv(descriptor, bytes + received, size - received, 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0)
            return false;
        if (errno == EINTR)
            continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_for_socket_event(descriptor, POLLIN))
            continue;
        return false;
    }
    return true;
}

bool verify_local_socket(const fs::path& path)
{
    const std::string socket_path = path.string();
    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path))
        return false;

    ScopedFd descriptor(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor.valid())
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
        return true;
    if (errno != EINPROGRESS || !wait_for_socket_event(descriptor.get(), POLLOUT))
        return false;

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    return ::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0
        && socket_error == 0;
}

bool verify_wayland_socket(const fs::path& path)
{
    const std::string socket_path = path.string();
    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path))
        return false;

    ScopedFd descriptor(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor.valid())
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS || !wait_for_socket_event(descriptor.get(), POLLOUT))
            return false;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 || socket_error != 0)
            return false;
    }

    const std::uint32_t request[] = {1U, (12U << 16U), 2U};
    if (!write_socket_data(descriptor.get(), request, sizeof(request)))
        return false;

    std::uint32_t response_header[2] {};
    if (!read_socket_data(descriptor.get(), response_header, sizeof(response_header)))
        return false;
    if (response_header[0] != 2U || (response_header[1] & 0xffffU) != 0U || (response_header[1] >> 16U) != 12U)
        return false;

    std::uint32_t callback_data = 0;
    return read_socket_data(descriptor.get(), &callback_data, sizeof(callback_data));
}

std::set<SocketIdentity> snapshot_wayland_sockets(const fs::path& runtime)
{
    std::set<SocketIdentity> sockets;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(runtime, error)) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("wayland-", 0) != 0)
            continue;
        if (const auto identity = socket_identity(entry.path()); identity.has_value())
            sockets.insert(*identity);
    }
    return sockets;
}

std::optional<std::pair<std::string, SocketIdentity>> find_new_wayland_socket(
    const fs::path& runtime,
    const std::set<SocketIdentity>& known_sockets)
{
    std::vector<fs::path> candidates;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(runtime, error)) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("wayland-", 0) == 0)
            candidates.push_back(entry.path());
    }

    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates) {
        const auto identity = socket_identity(candidate);
        if (!identity.has_value() || known_sockets.contains(*identity))
            continue;
        if (!verify_wayland_socket(candidate))
            continue;
        return std::make_pair(candidate.filename().string(), *identity);
    }

    return std::nullopt;
}


std::set<std::string> snapshot_hyprland_signatures(const fs::path& runtime)
{
    std::set<std::string> signatures;
    std::error_code error;
    const fs::path hyprland_root = runtime / "hypr";
    for (const auto& entry : fs::directory_iterator(hyprland_root, error)) {
        if (error)
            break;
        error.clear();
        if (!fs::is_directory(entry.path(), error) || error)
            continue;

        const std::string signature = entry.path().filename().string();
        if (signature.empty())
            continue;
        if (const auto socket = socket_identity(entry.path() / ".socket.sock"); socket.has_value())
            signatures.insert(signature);
    }
    return signatures;
}

bool is_safe_component(const std::string& value);

std::optional<std::string> find_new_hyprland_signature(
    const fs::path& runtime,
    const std::set<std::string>& known_signatures)
{
    std::vector<std::string> candidates;
    std::error_code error;
    const fs::path hyprland_root = runtime / "hypr";
    for (const auto& entry : fs::directory_iterator(hyprland_root, error)) {
        if (error)
            break;
        const std::string signature = entry.path().filename().string();
        if (!is_safe_component(signature) || known_signatures.contains(signature))
            continue;
        const fs::path socket_path = entry.path() / ".socket.sock";
        if (socket_identity(socket_path).has_value() && verify_local_socket(socket_path))
            candidates.push_back(signature);
    }

    std::sort(candidates.begin(), candidates.end());
    if (candidates.size() == 1)
        return candidates.front();
    return std::nullopt;
}

bool required_environment_name(const std::string& expected)
{
    auto configured = environment_value("NWSM_REQUIRED_VARS");
    if (!configured.has_value())
        configured = environment_value("NWSM_FINALIZE_REQUIRED_VARS");
    if (!configured.has_value())
        return false;

    std::size_t begin = 0;
    while (begin < configured->size()) {
        while (begin < configured->size()
            && ((*configured)[begin] == 32 || (*configured)[begin] == 9 || (*configured)[begin] == 44))
            ++begin;
        std::size_t end = begin;
        while (end < configured->size()
            && (*configured)[end] != 32 && (*configured)[end] != 9 && (*configured)[end] != 44)
            ++end;
        if (configured->substr(begin, end - begin) == expected)
            return true;
        begin = end;
    }
    return false;
}

bool is_safe_component(const std::string& value)
{
    if (value.empty() || value == "." || value == ".." || value.size() > 255)
        return false;

    for (const unsigned char character : value) {
        const bool alphanumeric = (character >= 48 && character <= 57)
            || (character >= 65 && character <= 90)
            || (character >= 97 && character <= 122);
        if (!alphanumeric && character != 45 && character != 46 && character != 95)
            return false;
    }
    return true;
}

constexpr const char* finalization_environment_names[] = {
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "XDG_SESSION_TYPE",
    "ELECTRON_OZONE_PLATFORM_HINT",
    "GDK_BACKEND",
    "GTK_USE_PORTAL",
    "QT_AUTO_SCREEN_SCALE_FACTOR",
    "QT_QPA_PLATFORM",
    "QT_QPA_PLATFORMTHEME",
    "SDL_VIDEODRIVER",
    "XCURSOR_SIZE",
    "XCURSOR_THEME",
    "HYPRLAND_INSTANCE_SIGNATURE",
    "HYPRLAND_CMD",
    "HYPRCURSOR_SIZE",
    "HYPRCURSOR_THEME",
};


bool is_finalization_environment_name(const std::string& name)
{
    for (const char* allowed : finalization_environment_names) {
        if (name == allowed)
            return true;
    }
    return false;
}

const std::string* finalized_value(
    const std::vector<std::pair<std::string, std::string>>& environment,
    const char* name)
{
    for (const auto& [key, value] : environment) {
        if (key == name)
            return &value;
    }
    return nullptr;
}

std::optional<std::vector<std::pair<std::string, std::string>>> read_finalization_file(
    const RuntimeDirectory& runtime)
{
    ScopedFd file(::openat(runtime.descriptor.get(), finalization_file_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file.valid())
        return std::nullopt;

    struct stat status {};
    if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0
        || status.st_size < 1 || status.st_size > 65536)
        return std::nullopt;

    std::string content(static_cast<std::size_t>(status.st_size), static_cast<char>(0));
    std::size_t received = 0;
    while (received < content.size()) {
        const ssize_t result = ::read(file.get(), content.data() + received, content.size() - received);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return std::nullopt;
    }

    for (const unsigned char byte : content) {
        if (byte == 0)
            return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> environment;
    std::size_t begin = 0;
    while (begin < content.size()) {
        const std::size_t end = content.find(static_cast<char>(10), begin);
        const std::size_t line_end = end == std::string::npos ? content.size() : end;
        const std::string line = content.substr(begin, line_end - begin);
        if (!line.empty()) {
            const std::size_t separator = line.find("=");
            if (separator == std::string::npos || separator == 0)
                return std::nullopt;

            const std::string name = line.substr(0, separator);
            const std::string value = line.substr(separator + 1);
            if (!is_finalization_environment_name(name)
                || value.find_first_of(static_cast<char>(13)) != std::string::npos
                || finalized_value(environment, name.c_str()) != nullptr)
                return std::nullopt;
            environment.emplace_back(name, value);
        }

        if (end == std::string::npos)
            break;
        begin = end + 1;
    }

    return environment;
}

bool finalization_is_valid(
    const RuntimeDirectory& runtime,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::string& expected_display,
    const std::optional<std::string>& expected_signature)
{
    const std::string* display = finalized_value(environment, "WAYLAND_DISPLAY");
    if (display == nullptr || *display != expected_display)
        return false;

    if (const std::string* signature = finalized_value(environment, "HYPRLAND_INSTANCE_SIGNATURE");
        signature != nullptr) {
        if (!is_safe_component(*signature)
            || (expected_signature.has_value() && *signature != *expected_signature))
            return false;
        if (!socket_identity(runtime.path / "hypr" / *signature / ".socket.sock").has_value())
            return false;
    }

    return true;
}

bool remove_finalization_file(const RuntimeDirectory& runtime)
{
    if (::unlinkat(runtime.descriptor.get(), finalization_file_name, 0) != 0 && errno != ENOENT) {
        log_message("could not remove the previous compositor environment: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

bool merge_finalized_environment(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    for (const auto& [name, value] : environment) {
        if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
            log_message(std::string("could not import ") + name + ": " + std::strerror(errno));
            return false;
        }
    }
    return true;
}

std::chrono::seconds finalization_grace()
{
    const auto value = environment_value("NWSM_FINALIZE_GRACE");
    if (!value.has_value())
        return default_finalize_grace;

    char* end = nullptr;
    errno = 0;
    const long seconds = std::strtol(value->c_str(), &end, 10);
    if (errno == 0 && end != value->c_str() && *end == 0 && seconds >= 0 && seconds <= 30)
        return std::chrono::seconds(seconds);
    return default_finalize_grace;
}

std::optional<std::vector<std::pair<std::string, std::string>>> wait_for_finalization(
    const RuntimeDirectory& runtime,
    const std::string& expected_display,
    const std::optional<std::string>& expected_signature,
    ChildState& child,
    std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!poll_session_process(child))
            return std::nullopt;

        if (const auto environment = read_finalization_file(runtime);
            environment.has_value()
            && finalization_is_valid(runtime, *environment, expected_display, expected_signature))
            return environment;
        std::this_thread::sleep_for(poll_interval);
    }
    return std::nullopt;
}

bool write_finalization_file()
{
    const auto runtime_value = environment_value("XDG_RUNTIME_DIR");
    if (!runtime_value.has_value())
        return false;
    const auto runtime = open_runtime_directory(*runtime_value);
    if (!runtime.has_value())
        return false;

    std::string content;
    bool has_wayland_display = false;
    for (const char* name : finalization_environment_names) {
        if (const char* value = std::getenv(name); value != nullptr) {
            if (std::strpbrk(value, "\r\n") != nullptr) {
                log_message("cannot finalize an environment containing a newline");
                return false;
            }
            content.append(name);
            content.append("=");
            content.append(value);
            content.append(1, static_cast<char>(10));
            if (std::string(name) == "WAYLAND_DISPLAY" && *value != '\0')
                has_wayland_display = true;
        }
    }

    if (!has_wayland_display) {
        log_message("cannot finalize without WAYLAND_DISPLAY");
        return false;
    }
    if (content.size() > 65536) {
        log_message("cannot finalize an environment larger than 64 KiB");
        return false;
    }

    const std::string temporary_name = std::string(finalization_file_name) + "." + std::to_string(::getpid());
    ScopedFd file(::openat(runtime->descriptor.get(), temporary_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!file.valid()) {
        log_message("could not create the compositor environment handoff: " + std::string(std::strerror(errno)));
        return false;
    }

    if (!write_all(file.get(), content.data(), content.size()) || ::fsync(file.get()) != 0) {
        ::unlinkat(runtime->descriptor.get(), temporary_name.c_str(), 0);
        log_message("could not write the compositor environment handoff");
        return false;
    }

    if (::renameat(runtime->descriptor.get(), temporary_name.c_str(),
        runtime->descriptor.get(), finalization_file_name) != 0) {
        ::unlinkat(runtime->descriptor.get(), temporary_name.c_str(), 0);
        log_message("could not publish the compositor environment handoff: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

int finalize_session()
{
    return write_finalization_file() ? 0 : 1;
}

bool wayland_socket_is_active(const fs::path& runtime, const std::string& display, const SocketIdentity& expected)
{
    const auto current = socket_identity(runtime / display);
    return current.has_value() && *current == expected;
}

std::chrono::seconds readiness_timeout()
{
    const auto value = environment_value("NWSM_READY_TIMEOUT");
    if (!value.has_value())
        return default_ready_timeout;

    char* end = nullptr;
    errno = 0;
    const long seconds = std::strtol(value->c_str(), &end, 10);
    if (errno == 0 && end != value->c_str() && *end == '\0' && seconds > 0 && seconds <= 600)
        return std::chrono::seconds(seconds);
    return default_ready_timeout;
}

std::optional<CompositorReadiness> wait_for_compositor_readiness(
    const fs::path& runtime,
    const std::set<SocketIdentity>& known_sockets,
    const std::set<std::string>& known_hyprland_signatures,
    ChildState& child,
    std::chrono::seconds timeout)
{
    const bool require_hyprland_signature = required_environment_name("HYPRLAND_INSTANCE_SIGNATURE");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<std::pair<std::string, SocketIdentity>> wayland;
    std::optional<std::string> hyprland_signature;

    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!poll_session_process(child))
            return std::nullopt;

        if (!wayland.has_value())
            wayland = find_new_wayland_socket(runtime, known_sockets);
        if (!hyprland_signature.has_value())
            hyprland_signature = find_new_hyprland_signature(runtime, known_hyprland_signatures);
        if (wayland.has_value() && (!require_hyprland_signature || hyprland_signature.has_value()))
            return CompositorReadiness{*wayland, hyprland_signature};

        std::this_thread::sleep_for(poll_interval);
    }
    return std::nullopt;
}

constexpr const char* activation_environment_names[] = {
    "DBUS_SESSION_BUS_ADDRESS",
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "XDG_SESSION_TYPE",
    "XDG_RUNTIME_DIR",
    "XDG_DATA_DIRS",
    "XDG_CONFIG_DIRS",
    "HOME",
    "PATH",
    "ELECTRON_OZONE_PLATFORM_HINT",
    "GDK_BACKEND",
    "GTK_USE_PORTAL",
    "QT_AUTO_SCREEN_SCALE_FACTOR",
    "QT_QPA_PLATFORM",
    "QT_QPA_PLATFORMTHEME",
    "SDL_VIDEODRIVER",
    "XCURSOR_SIZE",
    "XCURSOR_THEME",
    "HYPRLAND_INSTANCE_SIGNATURE",
    "HYPRLAND_CMD",
    "HYPRCURSOR_SIZE",
    "HYPRCURSOR_THEME",
};

EnvironmentSnapshot snapshot_activation_environment()
{
    EnvironmentSnapshot snapshot;
    for (const char* name : activation_environment_names) {
        if (const char* value = std::getenv(name); value != nullptr)
            snapshot.emplace_back(name, value);
    }
    return snapshot;
}

const std::string* snapshot_value(const EnvironmentSnapshot& snapshot, const char* name)
{
    for (const auto& [key, value] : snapshot) {
        if (key == name)
            return &value;
    }
    return nullptr;
}

bool update_activation_environment()
{
    const std::string updater = resolve_executable("dbus-update-activation-environment");
    if (updater.empty()) {
        log_message("dbus-update-activation-environment was not found");
        return false;
    }

    std::vector<std::string> arguments{updater};
    for (const char* name : activation_environment_names) {
        const char* value = std::getenv(name);
        arguments.emplace_back(std::string(name) + "=" + (value == nullptr ? "" : value));
    }
    if (run_command(arguments) != 0) {
        log_message("could not update the session D-Bus activation environment");
        return false;
    }
    return true;
}

void restore_activation_environment(const EnvironmentSnapshot& snapshot)
{
    const std::string updater = resolve_executable("dbus-update-activation-environment");
    if (updater.empty())
        return;

    std::vector<std::string> arguments{updater};
    for (const char* name : activation_environment_names) {
        const std::string* value = snapshot_value(snapshot, name);
        arguments.emplace_back(std::string(name) + "=" + (value == nullptr ? "" : *value));
    }
    run_command(arguments, true);
}

bool activate_desktop_runlevel(bool& attempted)
{
    attempted = false;
    const std::string openrc = resolve_executable("openrc");
    if (openrc.empty()) {
        log_message("openrc was not found");
        return false;
    }

    attempted = true;
    if (run_command({openrc, "-U", "desktop"}) != 0) {
        log_message("could not activate the desktop user runlevel");
        return false;
    }
    return true;
}

bool stop_desktop_runlevel()
{
    const std::string openrc = resolve_executable("openrc");
    if (openrc.empty()) {
        log_message("openrc was not found while stopping the desktop user runlevel");
        return false;
    }

    if (run_command({openrc, "-U", "shutdown"}) != 0) {
        log_message("could not stop the desktop user runlevel");
        return false;
    }
    return true;
}

const std::vector<std::string>& managed_user_services()
{
    static const std::vector<std::string> services{
        "dmemcg-booster",
        "gamemoded",
        "hyprscreend",
        "marina",
        "maui-bluetooth-obex-agent",
        "nudge-osd",
        "nx-apphubd",
        "nx-powerd",
        "openrazer-daemon",
        "pipewire",
        "pipewire-pulse",
        "polkit-nx-agent",
        "valenz",
        "vicinae",
        "wireplumber",
        "xdg-desktop-portal",
    };
    return services;
}

bool migrate_existing_user()
{
    const auto config_home = [&]() -> std::optional<fs::path> {
        if (const auto configured = environment_value("XDG_CONFIG_HOME"); configured.has_value()) {
            if (!configured->empty() && configured->front() == '/')
                return fs::path(*configured);
            return std::nullopt;
        }

        const auto home = environment_value("HOME");
        if (!home.has_value() || home->empty() || home->front() != '/')
            return std::nullopt;
        return fs::path(*home) / ".config";
    }();

    if (!config_home.has_value()) {
        log_message("cannot determine an absolute XDG_CONFIG_HOME for user migration");
        return false;
    }

    const fs::path rc_directory = *config_home / "rc";
    const fs::path desktop_runlevel = rc_directory / "runlevels" / "desktop";
    const fs::path marker = rc_directory / ".nwsm-migrated";
    std::error_code error;
    fs::create_directories(desktop_runlevel, error);
    if (error) {
        log_message("cannot create the user OpenRC configuration directory: " + error.message());
        return false;
    }

    const std::string rc_update = resolve_executable("rc-update");
    if (rc_update.empty()) {
        log_message("rc-update was not found for user service registration");
        return false;
    }

    bool success = true;
    for (const std::string& service : managed_user_services()) {
        const fs::path expected_target = fs::path("/etc/user/init.d") / service;
        std::error_code entry_error;
        if (!fs::is_regular_file(expected_target, entry_error) || entry_error) {
            log_message("the user service definition " + expected_target.string() + " is unavailable");
            success = false;
            continue;
        }

        const fs::path destination = desktop_runlevel / service;
        entry_error.clear();
        const fs::file_status destination_status = fs::symlink_status(destination, entry_error);
        const bool destination_missing = entry_error == std::errc::no_such_file_or_directory
            || destination_status.type() == fs::file_type::not_found;
        if (entry_error && !destination_missing) {
            log_message("cannot inspect the user service entry " + service + ": " + entry_error.message());
            success = false;
            continue;
        }

        if (!destination_missing) {
            bool destination_matches = false;
            bool replace_managed_link = false;
            if (fs::is_symlink(destination_status)) {
                entry_error.clear();
                destination_matches = fs::equivalent(destination, expected_target, entry_error);
                entry_error.clear();
                const fs::path target = fs::read_symlink(destination, entry_error);
                if (!entry_error) {
                    const fs::path target_parent = target.parent_path();
                    replace_managed_link = target == expected_target
                        || (target.filename() == service
                            && target_parent.filename() == "init.d"
                            && target_parent.parent_path().filename() == "user");
                }
            }

            if (destination_matches)
                continue;
            if (!replace_managed_link) {
                log_message("preserving the customized user service entry " + service);
                continue;
            }

            entry_error.clear();
            if (!fs::remove(destination, entry_error) || entry_error) {
                log_message("could not replace the legacy user service link " + service
                    + (entry_error ? ": " + entry_error.message() : ""));
                success = false;
                continue;
            }
        }

        if (run_command({rc_update, "-U", "add", service, "desktop"}) != 0) {
            log_message("could not add the user service " + service + " to the desktop runlevel");
            success = false;
        }
    }

    if (!success)
        return false;

    error.clear();
    const fs::file_status marker_status = fs::symlink_status(marker, error);
    if (!error && marker_status.type() == fs::file_type::regular)
        return true;
    if (error && error != std::errc::no_such_file_or_directory) {
        log_message("cannot inspect the existing-user migration marker: " + error.message());
        return false;
    }
    if (!error && marker_status.type() != fs::file_type::not_found) {
        log_message("the existing-user migration marker is not a regular file");
        return false;
    }

    ScopedFd marker_file(::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!marker_file.valid()) {
        log_message("could not create the existing-user migration marker: " + std::string(std::strerror(errno)));
        return false;
    }
    constexpr char marker_text[] = "Migrated by nwsm.\n";
    return write_all(marker_file.get(), marker_text, sizeof(marker_text) - 1);
}

bool set_session_environment()
{
    const std::vector<std::pair<const char*, const char*>> variables{
        {"ELECTRON_OZONE_PLATFORM_HINT", "auto"},
        {"GDK_BACKEND", "wayland,x11,*"},
        {"GTK_USE_PORTAL", "1"},
        {"QT_AUTO_SCREEN_SCALE_FACTOR", "1"},
        {"QT_QPA_PLATFORM", "wayland;xcb"},
        {"QT_QPA_PLATFORMTHEME", "kde"},
        {"SDL_VIDEODRIVER", "wayland"},
        {"HYPRCURSOR_SIZE", "24"},
        {"HYPRCURSOR_THEME", "nitrux_snow_cursors"},
        {"XCURSOR_SIZE", "24"},
        {"XCURSOR_THEME", "nitrux_snow_cursors"},
        {"XDG_CURRENT_DESKTOP", "Hyprland"},
        {"XDG_SESSION_DESKTOP", "Hyprland"},
        {"XDG_SESSION_TYPE", "wayland"},
    };

    bool success = true;
    for (const auto& [name, value] : variables) {
        if (::setenv(name, value, 1) != 0) {
            log_message(std::string("could not set ") + name + ": " + std::strerror(errno));
            success = false;
        }
    }

    constexpr const char* compositor_variables[] = {
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "HYPRLAND_INSTANCE_SIGNATURE",
        "HYPRLAND_CMD",
    };
    for (const char* name : compositor_variables) {
        if (::unsetenv(name) != 0) {
            log_message(std::string("could not clear ") + name + ": " + std::strerror(errno));
            success = false;
        }
    }

    return success;
}

bool apply_readiness_environment(const CompositorReadiness& readiness)
{
    if (!set_session_environment())
        return false;
    if (::setenv("WAYLAND_DISPLAY", readiness.wayland.first.c_str(), 1) != 0) {
        log_message("could not set WAYLAND_DISPLAY: " + std::string(std::strerror(errno)));
        return false;
    }
    if (readiness.hyprland_signature.has_value()
        && ::setenv("HYPRLAND_INSTANCE_SIGNATURE", readiness.hyprland_signature->c_str(), 1) != 0) {
        log_message("could not set HYPRLAND_INSTANCE_SIGNATURE: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

bool write_instance_pid(const ScopedFd& lock)
{
    const std::string text = std::to_string(::getpid()) + "\n";
    return ::ftruncate(lock.get(), 0) == 0
        && ::lseek(lock.get(), 0, SEEK_SET) == 0
        && write_all(lock.get(), text.data(), text.size())
        && ::fsync(lock.get()) == 0;
}

std::optional<pid_t> active_instance_pid(const RuntimeDirectory& runtime)
{
    ScopedFd lock(::openat(runtime.descriptor.get(), "nwsm.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    if (!lock.valid()) {
        if (errno == ENOENT)
            return pid_t{0};
        log_message("could not inspect the nwsm instance lock: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }

    struct stat status {};
    if (::fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid()) {
        log_message("the nwsm instance lock is not a regular file owned by the current user");
        return std::nullopt;
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) == 0) {
        ::flock(lock.get(), LOCK_UN);
        return pid_t{0};
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        log_message("could not inspect the nwsm instance lock: " + std::string(std::strerror(errno)));
        return std::nullopt;
    }

    char buffer[32] {};
    const ssize_t size = ::pread(lock.get(), buffer, sizeof(buffer) - 1, 0);
    if (size <= 0) {
        log_message("the active nwsm instance lock does not contain a PID");
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(buffer, &end, 10);
    if (errno != 0 || end == buffer || value <= 1) {
        log_message("the active nwsm instance lock contains an invalid PID");
        return std::nullopt;
    }
    return static_cast<pid_t>(value);
}

int control_instance(const RuntimeDirectory& runtime, const std::string& action)
{
    const auto pid = active_instance_pid(runtime);
    if (!pid.has_value())
        return 1;

    const bool active = *pid > 0;
    if (action == "check")
        return active ? 0 : 1;
    if (action == "status") {
        std::cout << (active ? "active" : "inactive") << "\n";
        return active ? 0 : 1;
    }
    if (!active)
        return 0;

    if (::kill(*pid, SIGTERM) != 0 && errno != ESRCH) {
        log_message("could not stop the active nwsm instance: " + std::string(std::strerror(errno)));
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = active_instance_pid(runtime);
        if (current.has_value() && *current == 0)
            return 0;
        std::this_thread::sleep_for(poll_interval);
    }
    log_message("timed out while stopping the active nwsm instance");
    return 1;
}

int session_process_exit_code(const ChildState& child)
{
    if (WIFEXITED(child.status))
        return WEXITSTATUS(child.status);
    if (WIFSIGNALED(child.status))
        return 128 + WTERMSIG(child.status);
    return 1;
}

int usage(const char* program)
{
    std::cerr << "Usage: " << program << " finalize\n"
              << "       " << program << " check|status|stop\n"
              << "       " << program << " -- <wayland-session-command> [arguments...]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "finalize")
        return finalize_session();

    const bool control_action = argc == 2
        && (std::string(argv[1]) == "check" || std::string(argv[1]) == "status" || std::string(argv[1]) == "stop");
    const bool dbus_child = argc >= 4 && std::string(argv[1]) == "--nwsm-dbus-child" && std::string(argv[2]) == "--";
    const int session_argument_start = dbus_child ? 3 : 2;
    if (!control_action
        && ((!dbus_child && (argc < 3 || std::string(argv[1]) != "--")) || (dbus_child && argc < 4)))
        return usage(argv[0]);

    const auto runtime_value = environment_value("XDG_RUNTIME_DIR");
    if (!runtime_value.has_value()) {
        log_message("XDG_RUNTIME_DIR is not available for the authenticated user");
        return 1;
    }
    const auto runtime_directory = open_runtime_directory(*runtime_value);
    if (!runtime_directory.has_value())
        return 1;

    if (control_action)
        return control_instance(*runtime_directory, argv[1]);

    if (!dbus_child) {
        const std::string dbus_run_session = resolve_executable("dbus-run-session");
        if (dbus_run_session.empty()) {
            log_message("dbus-run-session was not found");
            return 1;
        }

        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        if (error || !self.is_absolute()) {
            log_message("could not resolve the nwsm executable for the session D-Bus boundary");
            return 1;
        }

        if (::unsetenv("DBUS_SESSION_BUS_ADDRESS") != 0) {
            log_message("could not clear the inherited session D-Bus address");
            return 1;
        }

        std::vector<std::string> reexec{dbus_run_session, "--", self.string(), "--nwsm-dbus-child", "--"};
        for (int index = 2; index < argc; ++index)
            reexec.emplace_back(argv[index]);
        execute(reexec);
    }

    const EnvironmentSnapshot activation_environment = snapshot_activation_environment();
    if (!set_session_environment())
        return 1;

    const auto instance_lock = acquire_instance_lock(*runtime_directory);
    if (!instance_lock.has_value())
        return 1;
    if (!write_instance_pid(*instance_lock)) {
        log_message("could not record the active nwsm instance PID");
        return 1;
    }

    install_signal_handlers();

    if (!stop_desktop_runlevel())
        log_message("stale user services could not be fully reconciled; continuing session startup");
    if (!migrate_existing_user())
        log_message("existing-user service migration was incomplete; continuing session startup");
    if (!remove_finalization_file(*runtime_directory))
        log_message("could not remove a stale compositor environment handoff; continuing session startup");

    const fs::path runtime = runtime_directory->path;
    const std::set<SocketIdentity> known_wayland_sockets = snapshot_wayland_sockets(runtime);
    std::set<SocketIdentity> all_wayland_sockets = known_wayland_sockets;
    const std::set<std::string> known_hyprland_signatures = snapshot_hyprland_signatures(runtime);
    std::set<std::string> all_hyprland_signatures = known_hyprland_signatures;

    std::vector<std::string> session_arguments;
    for (int index = session_argument_start; index < argc; ++index)
        session_arguments.emplace_back(argv[index]);

    const auto session_process = spawn(session_arguments, false, true);
    if (!session_process.has_value()) {
        restore_activation_environment(activation_environment);
        return 1;
    }

    ChildState child{*session_process};
    session_process_pid = child.pid;
    session_process_pgid = child.pid;

    const auto timeout = readiness_timeout();
    const auto finalize_grace = finalization_grace();
    bool activation_environment_changed = false;

    const auto publish_readiness = [&](const CompositorReadiness& readiness) {
        if (!apply_readiness_environment(readiness))
            return false;

        if (finalize_grace.count() > 0) {
            const auto finalized = wait_for_finalization(
                *runtime_directory, readiness.wayland.first, readiness.hyprland_signature, child, finalize_grace);
            if (finalized.has_value()) {
                if (!merge_finalized_environment(*finalized))
                    return false;
                if (!remove_finalization_file(*runtime_directory))
                    log_message("could not consume the compositor environment handoff");
            }
            if (child.reaped || stop_requested)
                return false;
        }

        if (!update_activation_environment())
            log_message("activation environment publication was incomplete; continuing the graphical session");
        else
            activation_environment_changed = true;
        return true;
    };

    const auto initial_readiness = wait_for_compositor_readiness(
        runtime, all_wayland_sockets, all_hyprland_signatures, child, timeout);
    if (!initial_readiness.has_value() || !publish_readiness(*initial_readiness)) {
        if (!child.reaped)
            terminate_session_process(child);
        if (activation_environment_changed)
            restore_activation_environment(activation_environment);
        remove_finalization_file(*runtime_directory);
        return child.reaped ? session_process_exit_code(child) : 1;
    }
    all_wayland_sockets.insert(initial_readiness->wayland.second);
    if (initial_readiness->hyprland_signature.has_value())
        all_hyprland_signatures.insert(*initial_readiness->hyprland_signature);

    bool desktop_runlevel_active = false;
    if (!activate_desktop_runlevel(desktop_runlevel_active))
        log_message("desktop user services could not be fully activated; continuing the graphical session");

    CompositorReadiness active_readiness = *initial_readiness;
    while (!stop_requested && !child.reaped) {
        std::this_thread::sleep_for(poll_interval);
        if (!poll_session_process(child))
            break;

        const auto late_finalization = read_finalization_file(*runtime_directory);
        if (late_finalization.has_value()
            && finalization_is_valid(
                *runtime_directory, *late_finalization, active_readiness.wayland.first, active_readiness.hyprland_signature)) {
            if (merge_finalized_environment(*late_finalization)) {
                if (!update_activation_environment())
                    log_message("late compositor environment publication was incomplete");
                else
                    activation_environment_changed = true;
            }
            if (!remove_finalization_file(*runtime_directory))
                log_message("could not consume the late compositor environment handoff");
        }

        if (wayland_socket_is_active(
                runtime, active_readiness.wayland.first, active_readiness.wayland.second))
            continue;

        if (desktop_runlevel_active) {
            if (!stop_desktop_runlevel())
                log_message("desktop user services did not stop cleanly before compositor replacement; continuing recovery");
            desktop_runlevel_active = false;
        }
        if (!remove_finalization_file(*runtime_directory))
            log_message("could not remove the previous compositor environment handoff");

        const auto replacement_readiness = wait_for_compositor_readiness(
            runtime, all_wayland_sockets, all_hyprland_signatures, child, timeout);
        if (!replacement_readiness.has_value()) {
            log_message("replacement compositor did not become ready");
            terminate_session_process(child);
            break;
        }

        if (!publish_readiness(*replacement_readiness)) {
            terminate_session_process(child);
            break;
        }
        desktop_runlevel_active = false;
        if (!activate_desktop_runlevel(desktop_runlevel_active))
            log_message("desktop user services could not be reactivated; continuing the graphical session");

        all_wayland_sockets.insert(replacement_readiness->wayland.second);
        if (replacement_readiness->hyprland_signature.has_value())
            all_hyprland_signatures.insert(*replacement_readiness->hyprland_signature);
        active_readiness = *replacement_readiness;
    }

    if (session_process_pgid > 0 || !child.reaped)
        terminate_session_process(child);
    bool shutdown_failed = false;
    if (desktop_runlevel_active)
        shutdown_failed = !stop_desktop_runlevel();
    if (activation_environment_changed)
        restore_activation_environment(activation_environment);
    remove_finalization_file(*runtime_directory);

    if (stop_requested)
        return shutdown_failed ? 1 : 0;
    if (shutdown_failed)
        return 1;
    return session_process_exit_code(child);
}
