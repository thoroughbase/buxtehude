#include <buxtehude/buxtehude.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include <string_view>

#include <fmt/core.h>

#include <chrono>

namespace bux = buxtehude;

int main()
{
    tb::print("Starting test ({})\n", __FILE__);

    constexpr uint16_t PORT = 16370;
    constexpr std::string_view UNIX_FILE = "_unix_bux";

    bux::Initialise([] (auto level, auto msg) {
        if (level < bux::LogLevel::WARNING) return;
        tb::print("(buxtehude) {}\n", msg);
    });

    bux::Server server;

    auto fail_test = [&server] {
        tb::print("Test failed\n");
        server.Close();
        std::exit(1);
    };

    server.UnixServer(UNIX_FILE).if_err([fail_test] (bux::ListenError e) {
        tb::print("Failed to start unix server: {}\n", e.What());
        fail_test();
    }).if_ok([] {
        tb::print("Started UNIX server OK\n");
    });

    server.IPServer(PORT).if_err([fail_test] (bux::ListenError e) {
        tb::print("Failed to start inet server: {}\n", e.What());
        fail_test();
    }).if_ok([] {
        tb::print("Started INET server OK\n");
    });

    bux::Client client_ip({
        .teamname = "ip-client",
        .format = bux::MessageFormat::MSGPACK
    });

    bux::Client client_unix({
        .teamname = "unix-client",
        .format = bux::MessageFormat::JSON
    });

    bux::Client client_internal({
        .teamname = "internal-client"
    });

    // IP client

    bool ip_got_pong = false;

    client_ip.AddHandler("pong", [&ip_got_pong] (bux::Client&, const bux::Message&) {
        tb::print("ip-client received pong OK\n");
        ip_got_pong = true;
    });

    client_ip.IPConnect("localhost", PORT).if_err([fail_test] (bux::ConnectError e) {
        tb::print("ip-client failed to connect to inet server: {}\n", e.What());
        fail_test();
    }).if_ok([] {
        tb::print("ip-client connected to INET server OK\n");
    });

    // Unix client

    bool unix_got_ping = false;

    client_unix.AddHandler("ping",
      [&unix_got_ping, &fail_test] (bux::Client& c, const bux::Message&) {
        tb::print("unix-client received ping OK\n");
        unix_got_ping = true;
        c.Write({
            .type = "pong", .dest = "internal-client",
            .content = {
                { "target", "ip-client" }
            }
        }).if_err([&fail_test] (bux::WriteError) {
            tb::print("unix-client failed to write\n");
            fail_test();
        });
    });

    client_unix.UnixConnect(UNIX_FILE).if_err([&fail_test] (bux::ConnectError e) {
        tb::print("unix-client failed to connect to unix server: {}\n", e.What());
        fail_test();
    }).if_ok([] {
        tb::print("unix-client connected to UNIX server OK\n");
    });

    // Internal client

    client_internal.AddHandler("ping",
      [&fail_test] (bux::Client& c, const bux::Message& m) {
        tb::print("internal-client received ping from {} OK\n", m.src);
        c.Write({
            .type = "ping", .dest = m.content["target"]
        }).if_err([&fail_test] (bux::WriteError) {
            tb::print("internal-client failed to write\n");
            fail_test();
        });
    });

    client_internal.AddHandler("pong",
      [&fail_test] (bux::Client& c, const bux::Message& m) {
        tb::print("internal-client received pong from {} OK\n", m.src);
        c.Write({
            .type = "pong", .dest = m.content["target"]
        }).if_err([&fail_test] (bux::WriteError) {
            tb::print("internal-client failed to write\n");
            fail_test();
        });
    });

    client_internal.InternalConnect(server).if_err([&fail_test] (bux::ConnectError e) {
        tb::print("internal-client failed to connect to server: {}\n", e.What());
        fail_test();
    }).if_ok([] {
        tb::print("internal-client connected to server OK\n");
    });

    // Test ping pong

    using namespace std::chrono_literals;

    tb::print("Sleeping for 1s...\n");
    std::this_thread::sleep_for(1s);

    client_ip.Write({
        .type = "ping", .dest = "internal-client",
        .content = {
            { "target", "unix-client" }
        }
    }).if_err([&fail_test] (bux::WriteError) {
        tb::print("ip-client failed to write\n");
        fail_test();
    });

    tb::print("Sleeping for 1s...\n");
    std::this_thread::sleep_for(1s);

    assert(ip_got_pong && unix_got_ping);
    tb::print("Test ({}) completed successfully\n", __FILE__);

    return 0;
}
