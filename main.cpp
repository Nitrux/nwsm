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
constexpr std::chrono::seconds default_finalize_timeout{30};
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
    const std::set<std::string>& known_hyprland_signatures)
{
    const std::string* display = finalized_value(environment, "WAYLAND_DISPLAY");
    if (display == nullptr || *display != expected_display)
        return false;

    if (const std::string* signature = finalized_value(environment, "HYPRLAND_INSTANCE_SIGNATURE");
        signature != nullptr) {
        if (!is_safe_component(*signature) || known_hyprland_signatures.contains(*signature))
            return false;
        if (!socket_identity(runtime.path / "hypr" / *signature / ".socket.sock").has_value())
            return false;
    }

    if (const auto required = environment_value("NWSM_FINALIZE_REQUIRED_VARS"); required.has_value()) {
        std::size_t begin = 0;
        while (begin <= required->size()) {
            const std::size_t end = required->find(",", begin);
            const std::string name = required->substr(begin, end == std::string::npos ? end : end - begin);
            if (!is_finalization_environment_name(name)
                || finalized_value(environment, name.c_str()) == nullptr)
                return false;
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
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

bool apply_finalized_environment(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    bool success = true;
    for (const char* name : finalization_environment_names) {
        if (::unsetenv(name) != 0) {
            log_message(std::string("could not clear ") + name + ": " + std::strerror(errno));
            success = false;
        }
    }

    for (const auto& [name, value] : environment) {
        if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
            log_message(std::string("could not import ") + name + ": " + std::strerror(errno));
            success = false;
        }
    }
    return success;
}

std::chrono::seconds finalization_timeout()
{
    const auto value = environment_value("NWSM_FINALIZE_TIMEOUT");
    if (!value.has_value())
        return default_finalize_timeout;

    char* end = nullptr;
    errno = 0;
    const long seconds = std::strtol(value->c_str(), &end, 10);
    if (errno == 0 && end != value->c_str() && *end == 0 && seconds > 0 && seconds <= 600)
        return std::chrono::seconds(seconds);
    return default_finalize_timeout;
}

std::optional<std::vector<std::pair<std::string, std::string>>> wait_for_finalization(
    const RuntimeDirectory& runtime,
    const std::string& expected_display,
    const std::set<std::string>& known_hyprland_signatures,
    ChildState& child,
    std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!poll_session_process(child))
            return std::nullopt;

        if (const auto environment = read_finalization_file(runtime);
            environment.has_value()
            && finalization_is_valid(runtime, *environment, expected_display, known_hyprland_signatures))
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

std::optional<std::pair<std::string, SocketIdentity>> wait_for_new_wayland_socket(
    const fs::path& runtime,
    const std::set<SocketIdentity>& known_sockets,
    ChildState& child,
    std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!poll_session_process(child))
            return std::nullopt;

        if (const auto socket = find_new_wayland_socket(runtime, known_sockets); socket.has_value())
            return socket;
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

bool update_dbus_environment()
{
    const std::string updater = resolve_executable("dbus-update-activation-environment");
    if (updater.empty()) {
        log_message("dbus-update-activation-environment was not found");
        return false;
    }

    std::vector<std::string> arguments{updater};
    for (const char* name : activation_environment_names) {
        if (const char* value = std::getenv(name); value != nullptr)
            arguments.emplace_back(std::string(name) + "=" + value);
    }
    if (run_command(arguments) != 0) {
        log_message("could not update the session D-Bus activation environment");
        return false;
    }
    return true;
}

void clear_dbus_environment()
{
    const std::string updater = resolve_executable("dbus-update-activation-environment");
    if (updater.empty())
        return;

    std::vector<std::string> arguments{updater};
    for (const char* name : activation_environment_names) {
        arguments.emplace_back("--unset");
        arguments.emplace_back(name);
    }
    run_command(arguments, true);
}

bool activate_desktop_runlevel()
{
    const std::string openrc = resolve_executable("openrc");
    if (openrc.empty()) {
        log_message("openrc was not found");
        return false;
    }

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

bool remove_broken_user_service_links(const fs::path& config_home)
{
    const fs::path desktop_runlevel = config_home / "rc" / "runlevels" / "desktop";
    std::error_code error;
    if (!fs::exists(desktop_runlevel, error)) {
        if (error)
            log_message("cannot inspect the user desktop runlevel: " + error.message());
        return !error;
    }
    if (!fs::is_directory(desktop_runlevel, error)) {
        log_message("the user desktop runlevel is not a directory");
        return false;
    }

    for (const auto& entry : fs::directory_iterator(desktop_runlevel, error)) {
        if (error)
            break;
        error.clear();
        if (!fs::is_symlink(entry.path(), error)) {
            if (error) {
                log_message("cannot inspect the user desktop runlevel: " + error.message());
                return false;
            }
            continue;
        }

        const fs::path target = fs::read_symlink(entry.path(), error);
        if (error) {
            log_message("cannot read the user service link " + entry.path().filename().string() + ": " + error.message());
            return false;
        }
        if (!target.is_absolute() || target.parent_path() != fs::path("/etc/user/init.d"))
            continue;

        error.clear();
        if (fs::exists(target, error))
            continue;
        if (error) {
            log_message("cannot inspect user service target " + target.string() + ": " + error.message());
            return false;
        }
        if (!fs::remove(entry.path(), error)) {
            log_message("could not remove the broken user service link " + entry.path().filename().string() + ": " + error.message());
            return false;
        }
    }
    if (error) {
        log_message("cannot inspect the user desktop runlevel: " + error.message());
        return false;
    }
    return true;
}

std::optional<std::vector<std::string>> seed_user_services()
{
    const fs::path seed_runlevel = "/etc/skel/.config/rc/runlevels/desktop";
    std::error_code error;
    if (!fs::exists(seed_runlevel, error)) {
        if (error)
            log_message("cannot inspect the system desktop runlevel seed: " + error.message());
        return error ? std::nullopt : std::optional<std::vector<std::string>>{std::vector<std::string>{}};
    }
    if (!fs::is_directory(seed_runlevel, error)) {
        log_message("the system desktop runlevel seed is not a directory");
        return std::nullopt;
    }

    std::vector<std::string> services;
    for (const auto& entry : fs::directory_iterator(seed_runlevel, error)) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == 46)
            continue;
        error.clear();
        if (fs::is_symlink(entry.path(), error))
            services.push_back(name);
        else if (error) {
            log_message("cannot inspect the system desktop runlevel seed: " + error.message());
            return std::nullopt;
        }
    }
    if (error) {
        log_message("cannot inspect the system desktop runlevel seed: " + error.message());
        return std::nullopt;
    }

    std::sort(services.begin(), services.end());
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

    if (!remove_broken_user_service_links(*config_home))
        return false;

    const auto marker_status = fs::symlink_status(marker, error);
    const bool marker_missing = error == std::errc::no_such_file_or_directory;
    if (error && !marker_missing) {
        log_message("cannot inspect the existing-user migration marker: " + error.message());
        return false;
    }
    error.clear();
    bool marker_present = false;
    if (!marker_missing && marker_status.type() != fs::file_type::not_found) {
        if (marker_status.type() != fs::file_type::regular) {
            log_message("the existing-user migration marker is not a regular file");
            return false;
        }
        marker_present = true;
    }

    const auto seeded_services = seed_user_services();
    if (!seeded_services.has_value())
        return false;

    constexpr const char* seed_runlevel_path = "/etc/skel/.config/rc/runlevels/desktop";
    bool success = true;
    for (const std::string& service : *seeded_services) {
        const fs::path seed_entry = fs::path(seed_runlevel_path) / service;
        std::error_code link_error;
        const fs::path target = fs::read_symlink(seed_entry, link_error);
        if (link_error) {
            log_message("cannot read the seeded user service link " + service + ": " + link_error.message());
            success = false;
            continue;
        }
        if (!target.is_absolute() || target.parent_path() != fs::path("/etc/user/init.d")
            || target.filename() != service) {
            log_message("the seeded user service link " + service + " does not point to its full /etc/user/init.d path");
            success = false;
            continue;
        }

        const fs::path destination = desktop_runlevel / service;
        link_error.clear();
        const bool destination_is_symlink = fs::is_symlink(destination, link_error);
        if (link_error) {
            log_message("cannot inspect the user service link " + service + ": " + link_error.message());
            success = false;
            continue;
        }

        if (destination_is_symlink) {
            link_error.clear();
            if (fs::exists(destination, link_error))
                continue;
            if (link_error) {
                log_message("cannot inspect the user service link " + service + ": " + link_error.message());
                success = false;
                continue;
            }
        } else {
            link_error.clear();
            if (fs::exists(destination, link_error)) {
                if (!fs::remove(destination, link_error)) {
                    log_message("could not replace the regular user service entry " + service + ": " + link_error.message());
                    success = false;
                    continue;
                }
            } else if (link_error) {
                log_message("cannot inspect the user service entry " + service + ": " + link_error.message());
                success = false;
                continue;
            }
        }

        if (destination_is_symlink) {
            link_error.clear();
            if (!fs::remove(destination, link_error)) {
                log_message("could not replace the dangling user service link " + service + ": " + link_error.message());
                success = false;
                continue;
            }
        }

        link_error.clear();
        fs::create_symlink(target, destination, link_error);
        if (link_error) {
            log_message("could not create the user service link " + service + ": " + link_error.message());
            success = false;
        }
    }

    if (!success)
        return false;
    if (marker_present)
        return true;

    ScopedFd marker_file(::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!marker_file.valid()) {
        log_message("could not create the existing-user migration marker: " + std::string(std::strerror(errno)));
        return false;
    }
    constexpr char marker_text[] = "Migrated by nwsm.\n";
    std::size_t written = 0;
    while (written < sizeof(marker_text) - 1) {
        const ssize_t result = ::write(marker_file.get(), marker_text + written, sizeof(marker_text) - 1 - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        log_message("could not write the existing-user migration marker: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
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
        {"XCURSOR_SIZE", "24"},
        {"XCURSOR_THEME", "nitrux_snow_cursors"},
        {"XDG_SESSION_TYPE", "wayland"},
    };

    bool success = true;
    for (const auto& [name, value] : variables) {
        if (::setenv(name, value, 1) != 0) {
            log_message(std::string("could not set ") + name + ": " + std::strerror(errno));
            success = false;
        }
    }

    if (::unsetenv("WAYLAND_DISPLAY") != 0) {
        log_message("could not clear WAYLAND_DISPLAY: " + std::string(std::strerror(errno)));
        success = false;
    }

    return success;
}

bool set_wayland_environment(const std::string& display)
{
    if (::setenv("WAYLAND_DISPLAY", display.c_str(), 1) != 0) {
        log_message("could not set WAYLAND_DISPLAY: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
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
              << "       " << program << " -- <wayland-session-command> [arguments...]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "finalize")
        return finalize_session();

    const bool dbus_child = argc >= 4 && std::string(argv[1]) == "--nwsm-dbus-child" && std::string(argv[2]) == "--";
    const int session_argument_start = dbus_child ? 3 : 2;
    if ((!dbus_child && (argc < 3 || std::string(argv[1]) != "--")) || (dbus_child && argc < 4))
        return usage(argv[0]);

    const auto runtime_value = environment_value("XDG_RUNTIME_DIR");
    if (!runtime_value.has_value())
        return 1;
    const auto runtime_directory = open_runtime_directory(*runtime_value);
    if (!runtime_directory.has_value())
        return 1;

    if (!set_session_environment())
        return 1;

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

    const auto instance_lock = acquire_instance_lock(*runtime_directory);
    if (!instance_lock.has_value())
        return 1;

    install_signal_handlers();
    if (!migrate_existing_user())
        return 1;

    const fs::path runtime = runtime_directory->path;
    if (!remove_finalization_file(*runtime_directory))
        return 1;

    const std::set<SocketIdentity> known_wayland_sockets = snapshot_wayland_sockets(runtime);
    std::set<SocketIdentity> all_wayland_sockets = known_wayland_sockets;
    const std::set<std::string> known_hyprland_signatures = snapshot_hyprland_signatures(runtime);
    std::set<std::string> all_hyprland_signatures = known_hyprland_signatures;

    std::vector<std::string> session_arguments;
    for (int index = session_argument_start; index < argc; ++index)
        session_arguments.emplace_back(argv[index]);

    const auto session_process = spawn(session_arguments, false, true);
    if (!session_process.has_value())
        return 1;

    ChildState child{*session_process};
    session_process_pid = child.pid;
    session_process_pgid = child.pid;

    const auto timeout = readiness_timeout();
    const auto finalize_wait = finalization_timeout();
    const auto initial_wayland = wait_for_new_wayland_socket(runtime, all_wayland_sockets, child, timeout);
    if (!initial_wayland.has_value()) {
        terminate_session_process(child);
        return child.reaped ? session_process_exit_code(child) : 1;
    }
    all_wayland_sockets.insert(initial_wayland->second);

    if (!set_wayland_environment(initial_wayland->first)) {
        terminate_session_process(child);
        return 1;
    }
    const auto initial_environment = wait_for_finalization(
        *runtime_directory, initial_wayland->first, all_hyprland_signatures, child, finalize_wait);
    if (!initial_environment.has_value()) {
        log_message("compositor finalization was not received");
        terminate_session_process(child);
        clear_dbus_environment();
        return child.reaped ? session_process_exit_code(child) : 1;
    }
    if (!apply_finalized_environment(*initial_environment) || !update_dbus_environment()) {
        terminate_session_process(child);
        clear_dbus_environment();
        return 1;
    }
    if (const std::string* signature = finalized_value(*initial_environment, "HYPRLAND_INSTANCE_SIGNATURE");
        signature != nullptr)
        all_hyprland_signatures.insert(*signature);

    if (!activate_desktop_runlevel()) {
        stop_desktop_runlevel();
        terminate_session_process(child);
        clear_dbus_environment();
        return 1;
    }
    bool desktop_runlevel_active = true;

    std::pair<std::string, SocketIdentity> active_wayland = *initial_wayland;
    while (!stop_requested && !child.reaped) {
        std::this_thread::sleep_for(poll_interval);
        if (!poll_session_process(child))
            break;

        if (wayland_socket_is_active(runtime, active_wayland.first, active_wayland.second))
            continue;

        if (desktop_runlevel_active) {
            if (!stop_desktop_runlevel()) {
                terminate_session_process(child);
                break;
            }
            desktop_runlevel_active = false;
        }
        if (!remove_finalization_file(*runtime_directory)) {
            terminate_session_process(child);
            break;
        }

        const auto replacement_wayland = wait_for_new_wayland_socket(runtime, all_wayland_sockets, child, timeout);
        if (!replacement_wayland.has_value()) {
            log_message("replacement compositor did not create a new Wayland socket");
            terminate_session_process(child);
            break;
        }
        all_wayland_sockets.insert(replacement_wayland->second);

        if (!set_wayland_environment(replacement_wayland->first)) {
            terminate_session_process(child);
            break;
        }

        const auto replacement_environment = wait_for_finalization(
            *runtime_directory, replacement_wayland->first, all_hyprland_signatures, child, finalize_wait);
        if (!replacement_environment.has_value()) {
            log_message("replacement compositor finalization was not received");
            terminate_session_process(child);
            break;
        }
        if (!apply_finalized_environment(*replacement_environment) || !update_dbus_environment()
            || !activate_desktop_runlevel()) {
            terminate_session_process(child);
            break;
        }
        if (const std::string* signature = finalized_value(*replacement_environment, "HYPRLAND_INSTANCE_SIGNATURE");
            signature != nullptr)
            all_hyprland_signatures.insert(*signature);

        desktop_runlevel_active = true;
        active_wayland = *replacement_wayland;
    }

    if (session_process_pgid > 0 || !child.reaped)
        terminate_session_process(child);
    bool shutdown_failed = false;
    if (desktop_runlevel_active)
        shutdown_failed = !stop_desktop_runlevel();
    clear_dbus_environment();
    remove_finalization_file(*runtime_directory);

    if (stop_requested)
        return shutdown_failed ? 1 : 0;
    if (shutdown_failed)
        return 1;
    return session_process_exit_code(child);
}
