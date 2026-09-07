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
    const ssize_t written = ::write(fd, content.data(), content.size());
    ::close(fd);
    require(written == static_cast<ssize_t>(content.size()), "could not write test file");
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

} // namespace

int main()
{
    try {
        test_environment_lists();
        test_dynamic_xdg_collection();
        test_service_registration_lifecycle();
        test_service_marker_migration();
        test_wayland_socket_replacement();
        test_failed_readiness();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nwsm environment test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
