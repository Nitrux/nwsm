#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define NWSM_TESTING
#define main nwsm_entrypoint
#include "../main.cpp"
#undef main

namespace {

bool contains(const std::vector<std::string>& values, const std::string& expected)
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

std::size_t count(const std::vector<std::string>& values, const std::string& expected)
{
    return static_cast<std::size_t>(std::count(values.begin(), values.end(), expected));
}

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void write_file(const std::filesystem::path& path, const std::string& content)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    require(fd >= 0, "could not create test file");
    require(::fchmod(fd, 0600) == 0, "could not secure test file");
    const ssize_t written = ::write(fd, content.data(), content.size());
    ::close(fd);
    require(written == static_cast<ssize_t>(content.size()), "could not write test file");
}

class TestOverrides {
public:
    TestOverrides(TestCommandRunner command_runner, TestExecutableResolver executable_resolver,
        const std::filesystem::path& user_service_directory)
    {
        test_command_runner = command_runner;
        test_executable_resolver = executable_resolver;
        test_user_service_directory = user_service_directory;
    }

    ~TestOverrides()
    {
        test_command_runner = nullptr;
        test_executable_resolver = nullptr;
        test_user_service_directory.clear();
    }

    TestOverrides(const TestOverrides&) = delete;
    TestOverrides& operator=(const TestOverrides&) = delete;
};

std::vector<std::vector<std::string>> lifecycle_commands;

int record_lifecycle_command(const std::vector<std::string>& arguments, bool)
{
    lifecycle_commands.push_back(arguments);
    return 0;
}

std::string resolve_test_executable(const std::string& executable)
{
    return "/usr/bin/" + executable;
}

int run_test_compositor()
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr)
        return 1;

    const std::filesystem::path socket_path = std::filesystem::path(runtime) / "wayland-nwsm-test";
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return 1;

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.string();
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(descriptor);
        return 1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ::unlink(socket_path.c_str());
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(descriptor, 1) != 0) {
        ::close(descriptor);
        return 1;
    }

    const int client = ::accept(descriptor, nullptr, nullptr);
    if (client < 0) {
        ::close(descriptor);
        ::unlink(socket_path.c_str());
        return 1;
    }

    std::uint8_t request[12] {};
    std::size_t received = 0;
    while (received < sizeof(request)) {
        const ssize_t result = ::read(client, request + received, sizeof(request) - received);
        if (result <= 0)
            break;
        received += static_cast<std::size_t>(result);
    }
    if (received == sizeof(request)) {
        const std::uint32_t response[] = {2U, (12U << 16U), 0U};
        ::write(client, response, sizeof(response));
    }
    ::close(client);
    ::usleep(300000);
    ::close(descriptor);
    return received == sizeof(request) ? 0 : 1;
}

class TestWaylandSocket {
public:
    TestWaylandSocket(const std::filesystem::path& path, bool healthy)
        : m_path(path)
        , m_healthy(healthy)
    {
        m_descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(m_descriptor >= 0, "could not create test Wayland socket");

        struct sockaddr_un address {};
        address.sun_family = AF_UNIX;
        const std::string socket_path = m_path.string();
        require(socket_path.size() < sizeof(address.sun_path), "test Wayland socket path is too long");
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
        ::unlink(m_path.c_str());
        require(::bind(m_descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "could not bind test Wayland socket");
        require(::listen(m_descriptor, 1) == 0, "could not listen on test Wayland socket");

        m_thread = std::thread([this] {
            const int client = ::accept(m_descriptor, nullptr, nullptr);
            if (client < 0)
                return;
            if (m_healthy) {
                std::uint8_t request[12] {};
                std::size_t received = 0;
                while (received < sizeof(request)) {
                    const ssize_t result = ::read(client, request + received, sizeof(request) - received);
                    if (result <= 0)
                        break;
                    received += static_cast<std::size_t>(result);
                }
                if (received == sizeof(request)) {
                    const std::uint32_t response[] = {2U, (12U << 16U), 0U};
                    ::write(client, response, sizeof(response));
                }
            }
            ::close(client);
        });
    }

    ~TestWaylandSocket()
    {
        if (m_thread.joinable())
            m_thread.join();
        if (m_descriptor >= 0)
            ::close(m_descriptor);
        ::unlink(m_path.c_str());
    }

    TestWaylandSocket(const TestWaylandSocket&) = delete;
    TestWaylandSocket& operator=(const TestWaylandSocket&) = delete;

private:
    std::filesystem::path m_path;
    bool m_healthy;
    int m_descriptor{-1};
    std::thread m_thread;
};

void test_environment_lists()
{
    require(std::all_of(std::begin(base_finalization_environment_names),
        std::end(base_finalization_environment_names), [](const char* name) {
            return name != nullptr && *name != '\0';
        }), "finalization environment list contains an invalid entry");
    require(std::all_of(std::begin(base_activation_environment_names),
        std::end(base_activation_environment_names), [](const char* name) {
            return name != nullptr && *name != '\0';
        }), "activation environment list contains an invalid entry");

    const auto finalization = current_finalization_environment_names();
    for (const char* name : base_finalization_environment_names)
        require(count(finalization, name) == 1, "finalization base entry is missing or duplicated");

    const auto activation = activation_environment_names();
    for (const char* name : base_activation_environment_names)
        require(count(activation, name) == 1, "activation base entry is missing or duplicated");
}

void test_dynamic_xdg_collection()
{
    ::setenv("XDG_NWSM_TEST_VALUE", "present", 1);
    ::setenv("XDG_ACTIVATION_TOKEN", "transient", 1);

    const auto finalization = current_finalization_environment_names();
    const auto activation = activation_environment_names();
    require(count(finalization, "XDG_NWSM_TEST_VALUE") == 1, "dynamic XDG finalization value was not collected");
    require(count(activation, "XDG_NWSM_TEST_VALUE") == 1, "dynamic XDG activation value was not collected");
    require(!contains(finalization, "XDG_ACTIVATION_TOKEN"), "transient activation token was finalized");
    require(!contains(activation, "XDG_ACTIVATION_TOKEN"), "transient activation token was activated");

    EnvironmentSnapshot previous{{"XDG_REMOVED_NWSM_TEST", "old"}, {"XDG_ACTIVATION_TOKEN", "old-token"}};
    ::setenv("XDG_REMOVED_NWSM_TEST", "current", 1);
    const auto with_current = activation_environment_names(&previous);
    ::unsetenv("XDG_REMOVED_NWSM_TEST");
    const auto with_removed = activation_environment_names(&previous);
    require(count(with_current, "XDG_REMOVED_NWSM_TEST") == 1, "dynamic XDG value was duplicated");
    require(count(with_removed, "XDG_REMOVED_NWSM_TEST") == 1, "removed XDG value was not retained for restoration");
    require(!contains(with_removed, "XDG_ACTIVATION_TOKEN"), "transient token was retained for restoration");

    const auto restoration_arguments = activation_environment_arguments("/usr/bin/test", &previous);
    require(contains(restoration_arguments, "XDG_REMOVED_NWSM_TEST=old"),
        "activation restoration did not preserve a removed XDG value");
    require(!contains(restoration_arguments, "XDG_ACTIVATION_TOKEN=old-token"),
        "activation restoration included a transient token");

    ::unsetenv("XDG_NWSM_TEST_VALUE");
    ::unsetenv("XDG_ACTIVATION_TOKEN");
}

void test_service_registration_lifecycle()
{
    char directory_template[] = "/tmp/nwsm-service-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create service test directory");
    const std::filesystem::path init_directory = std::filesystem::path(directory_name) / "init.d";
    std::filesystem::create_directory(init_directory);
    write_file(init_directory / "existing", "#!/bin/sh\n");
    write_file(init_directory / "new-service", "#!/bin/sh\n");
    write_file(init_directory / ".hidden", "ignored\n");

    const auto discovered = installed_user_services(init_directory);
    require(discovered.has_value()
            && *discovered == std::vector<std::string>{"existing", "new-service"},
        "installed user service discovery is incorrect");
    require(services_requiring_registration(*discovered, {})
            == std::set<std::string>{"existing", "new-service"},
        "first-install registration plan is incorrect");
    require(services_requiring_registration(*discovered, {"existing"})
            == std::set<std::string>{"new-service"},
        "new-service registration plan is incorrect");
    require(services_requiring_registration(*discovered, {"existing", "new-service"}).empty(),
        "user-disabled services would be re-enabled");

    std::set<std::string> migrated{legacy_service_registration_entry};
    migrated.insert(discovered->begin(), discovered->end());
    require(services_requiring_registration(*discovered, migrated).empty(),
        "legacy marker migration would re-register existing services");

    std::filesystem::remove_all(directory_name);
}

void test_service_marker_migration()
{
    char directory_template[] = "/tmp/nwsm-marker-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create marker test directory");
    const std::filesystem::path marker = std::filesystem::path(directory_name) / "marker";

    write_file(marker, legacy_service_registration_marker);
    const auto legacy = read_service_registration_marker(marker);
    require(legacy.has_value() && legacy->contains(legacy_service_registration_entry),
        "legacy service marker was not recognized");

    const std::set<std::string> services{"marina", "valenz"};
    require(write_service_registration_marker(marker, services), "could not write modern service marker");
    const auto modern = read_service_registration_marker(marker);
    require(modern.has_value() && *modern == services, "modern service marker did not round-trip");

    std::filesystem::remove_all(directory_name);
}

void test_finalization_file_validation()
{
    char directory_template[] = "/tmp/nwsm-finalization-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create finalization test directory");
    const auto runtime = open_runtime_directory(directory_name);
    require(runtime.has_value(), "could not open finalization test directory");
    const std::filesystem::path handoff = std::filesystem::path(directory_name) / finalization_file_name;

    write_file(handoff, "WAYLAND_DISPLAY=wayland-1\nXDG_SESSION_ID=7\n");
    const auto valid = read_finalization_file(*runtime);
    require(valid.has_value(), "valid finalization file was rejected");
    require(finalization_is_valid(*runtime, *valid, "wayland-1", std::nullopt),
        "valid finalization environment was rejected");
    require(!finalization_is_valid(*runtime, *valid, "wayland-2", std::nullopt),
        "mismatched Wayland display was accepted");

    write_file(handoff, "WAYLAND_DISPLAY=wayland-1\nBAD-NAME=value\n");
    require(!read_finalization_file(*runtime).has_value(), "malformed environment name was accepted");

    write_file(handoff, "WAYLAND_DISPLAY=wayland-1\nWAYLAND_DISPLAY=wayland-2\n");
    require(!read_finalization_file(*runtime).has_value(), "duplicate finalization entry was accepted");

    write_file(handoff, "WAYLAND_DISPLAY=wayland-1\nXDG_SESSION_ID=bad\rvalue\n");
    require(!read_finalization_file(*runtime).has_value(), "carriage return in finalization value was accepted");

    write_file(handoff, "WAYLAND_DISPLAY=wayland-1\nXDG_ACTIVATION_TOKEN=transient\n");
    require(!read_finalization_file(*runtime).has_value(), "transient activation token was accepted from handoff");

    const std::filesystem::path hyprland_directory = std::filesystem::path(directory_name) / "hypr" / "signature";
    std::filesystem::create_directories(hyprland_directory);
    const std::filesystem::path hyprland_socket = hyprland_directory / ".socket.sock";
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(descriptor >= 0, "could not create Hyprland test socket");
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string socket_path = hyprland_socket.string();
    require(socket_path.size() < sizeof(address.sun_path), "Hyprland test socket path is too long");
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    require(::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
        "could not bind Hyprland test socket");

    const std::vector<std::pair<std::string, std::string>> matching_signature{
        {"WAYLAND_DISPLAY", "wayland-1"}, {"HYPRLAND_INSTANCE_SIGNATURE", "signature"}};
    require(finalization_is_valid(*runtime, matching_signature, "wayland-1", std::string("signature")),
        "matching Hyprland signature was rejected");
    require(!finalization_is_valid(*runtime, matching_signature, "wayland-1", std::string("different")),
        "mismatched Hyprland signature was accepted");

    const std::vector<std::pair<std::string, std::string>> forged_signature{
        {"WAYLAND_DISPLAY", "wayland-1"}, {"HYPRLAND_INSTANCE_SIGNATURE", "../signature"}};
    require(!finalization_is_valid(*runtime, forged_signature, "wayland-1", std::nullopt),
        "forged Hyprland signature was accepted");

    const std::vector<std::pair<std::string, std::string>> missing_socket{
        {"WAYLAND_DISPLAY", "wayland-1"}, {"HYPRLAND_INSTANCE_SIGNATURE", "missing"}};
    require(!finalization_is_valid(*runtime, missing_socket, "wayland-1", std::string("missing")),
        "Hyprland signature without its socket was accepted");

    ::close(descriptor);
    std::filesystem::remove_all(directory_name);
}

void test_wayland_socket_replacement()
{
    char directory_template[] = "/tmp/nwsm-wayland-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create Wayland test directory");
    const std::filesystem::path runtime = directory_name;
    const std::filesystem::path first_path = runtime / "wayland-1";
    const std::filesystem::path replacement_path = runtime / "wayland-2";
    std::set<SocketIdentity> known;
    SocketIdentity first_identity{};

    {
        TestWaylandSocket server(first_path, true);
        const auto detected = find_new_wayland_socket(runtime, known);
        require(detected.has_value() && detected->first == "wayland-1", "new Wayland socket was not detected");
        first_identity = detected->second;
        known.insert(first_identity);
    }

    {
        TestWaylandSocket failed_server(replacement_path, false);
        require(!find_new_wayland_socket(runtime, known).has_value(), "unresponsive Wayland socket was accepted");
    }

    {
        TestWaylandSocket replacement_server(replacement_path, true);
        const auto replacement = find_new_wayland_socket(runtime, known);
        require(replacement.has_value() && replacement->first == "wayland-2",
            "replacement Wayland socket was not detected");
        require(!(replacement->second == first_identity), "replacement socket reused the previous identity");
    }

    ::rmdir(directory_name);
}

void test_failed_readiness()
{
    char directory_template[] = "/tmp/nwsm-readiness-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create readiness test directory");
    ::unsetenv("NWSM_REQUIRED_VARS");
    ::unsetenv("NWSM_FINALIZE_REQUIRED_VARS");

    const pid_t pid = ::fork();
    require(pid >= 0, "could not create failed compositor test process");
    if (pid == 0)
        _exit(7);

    ChildState child{pid};
    const auto readiness = wait_for_compositor_readiness(
        directory_name, {}, {}, child, std::chrono::seconds(1));
    require(!readiness.has_value() && child.reaped, "failed compositor readiness was not reported");
    ::rmdir(directory_name);
}

void test_restart_policy()
{
    ::unsetenv("NWSM_RESTART_ON_EXIT");
    require(restart_session_on_exit(), "session restart should be enabled by default");
    ::setenv("NWSM_RESTART_ON_EXIT", "0", 1);
    require(!restart_session_on_exit(), "session restart should be disableable");
    ::setenv("NWSM_RESTART_ON_EXIT", "false", 1);
    require(!restart_session_on_exit(), "false should disable session restart");
    ::setenv("NWSM_RESTART_ON_EXIT", "yes", 1);
    require(restart_session_on_exit(), "yes should enable session restart");
    ::unsetenv("NWSM_RESTART_ON_EXIT");
}

void test_reaped_session_process_release()
{
    ChildState child;
    child.reaped = true;
    session_process_pid = 123;
    session_process_pgid = ::getpgrp();

    release_reaped_session_process(child);

    require(session_process_pid == -1 && session_process_pgid == -1,
        "reaped session process tracking was not released");
    require(std::find(retained_session_process_groups.begin(), retained_session_process_groups.end(), ::getpgrp())
            != retained_session_process_groups.end(),
        "surviving session process group was not retained for logout");
    retained_session_process_groups.clear();
}

void test_wrapper_recovery_preserves_descendants()
{
    const pid_t wrapper = ::fork();
    require(wrapper >= 0, "could not create session wrapper test process");
    if (wrapper == 0) {
        if (::setpgid(0, 0) != 0)
            _exit(2);
        ::pause();
        _exit(0);
    }
    require(::setpgid(wrapper, wrapper) == 0 || errno == EACCES,
        "could not create the session wrapper process group");

    const pid_t descendant = ::fork();
    require(descendant >= 0, "could not create session descendant test process");
    if (descendant == 0) {
        if (::setpgid(0, wrapper) != 0)
            _exit(2);
        ::pause();
        _exit(0);
    }
    require(::setpgid(descendant, wrapper) == 0 || errno == EACCES,
        "could not place the session descendant in the wrapper process group");

    ChildState child{wrapper};
    session_process_pid = wrapper;
    session_process_pgid = wrapper;
    terminate_session_command(child);

    require(child.reaped, "session wrapper was not reaped during recovery");
    errno = 0;
    require(::kill(descendant, 0) == 0 || errno == EPERM,
        "recovery terminated a surviving session descendant");

    release_reaped_session_process(child);
    require(std::find(retained_session_process_groups.begin(), retained_session_process_groups.end(), wrapper)
            != retained_session_process_groups.end(),
        "recovery did not retain the descendant process group");

    ::kill(descendant, SIGTERM);
    while (::waitpid(descendant, nullptr, 0) < 0 && errno == EINTR) {
    }
    retained_session_process_groups.clear();
}

void test_session_lifecycle()
{
    char directory_template[] = "/tmp/nwsm-lifecycle-test.XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    require(directory_name != nullptr, "could not create lifecycle test directory");
    const std::filesystem::path root = directory_name;
    const std::filesystem::path runtime = root / "runtime";
    const std::filesystem::path config_home = root / "config";
    const std::filesystem::path init_directory = root / "init.d";
    std::filesystem::create_directory(runtime);
    std::filesystem::create_directory(config_home);
    std::filesystem::create_directory(init_directory);
    require(::chmod(runtime.c_str(), 0700) == 0, "could not secure lifecycle runtime directory");
    write_file(init_directory / "test-desktop-service", "#!/bin/sh\n");

    std::vector<std::string> arguments{
        "nwsm-test", "--nwsm-dbus-child", "--", "/proc/self/exe", "--test-compositor"};
    std::vector<char*> argv;
    for (std::string& argument : arguments)
        argv.push_back(argument.data());

    lifecycle_commands.clear();
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    ::setenv("NWSM_FINALIZE_GRACE", "0", 1);
    ::setenv("NWSM_RESTART_ON_EXIT", "0", 1);
    ::unsetenv("DISPLAY");
    ::unsetenv("WAYLAND_DISPLAY");
    ::unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    ::unsetenv("HYPRLAND_CMD");
    ::unsetenv("NWSM_REQUIRED_VARS");
    ::unsetenv("NWSM_FINALIZE_REQUIRED_VARS");

    {
        TestOverrides overrides(record_lifecycle_command, resolve_test_executable, init_directory);
        require(nwsm_entrypoint(static_cast<int>(argv.size()), argv.data()) == 0,
            "controlled nwsm session lifecycle failed");
    }

    require(lifecycle_commands.size() == 6, "session lifecycle issued an unexpected number of commands");
    require(lifecycle_commands[0] == std::vector<std::string>{"/usr/bin/openrc", "-U", "shutdown"},
        "session lifecycle did not reconcile stale services first");
    require(lifecycle_commands[1] == std::vector<std::string>{
            "/usr/bin/rc-update", "-U", "add", "test-desktop-service", "desktop"},
        "session lifecycle did not enroll the discovered service");
    require(lifecycle_commands[2].front() == "/usr/bin/dbus-update-activation-environment"
            && contains(lifecycle_commands[2], "WAYLAND_DISPLAY=wayland-nwsm-test"),
        "session lifecycle did not publish compositor readiness");
    require(lifecycle_commands[3] == std::vector<std::string>{"/usr/bin/openrc", "-U", "desktop"},
        "session lifecycle did not activate desktop services");
    require(lifecycle_commands[4] == std::vector<std::string>{"/usr/bin/openrc", "-U", "shutdown"},
        "session lifecycle did not stop desktop services");
    require(lifecycle_commands[5].front() == "/usr/bin/dbus-update-activation-environment"
            && contains(lifecycle_commands[5], "WAYLAND_DISPLAY="),
        "session lifecycle did not restore the activation environment");

    ::unsetenv("NWSM_FINALIZE_GRACE");
    ::unsetenv("NWSM_RESTART_ON_EXIT");
    std::filesystem::remove_all(directory_name);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--test-compositor")
        return run_test_compositor();

    try {
        test_environment_lists();
        test_dynamic_xdg_collection();
        test_service_registration_lifecycle();
        test_service_marker_migration();
        test_finalization_file_validation();
        test_restart_policy();
        test_reaped_session_process_release();
        test_wrapper_recovery_preserves_descendants();
        test_wayland_socket_replacement();
        test_failed_readiness();
        test_session_lifecycle();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nwsm environment test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
