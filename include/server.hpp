#pragma once

#include "core.hpp"
#include "stream.hpp"

#include <tb/tb.h>

#include <ctime>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <event2/event.h>
#include <event2/listener.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace buxtehude
{

class Client;

class ClientHandle
{
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
public:
    ClientHandle(Client& iclient, std::string_view teamname);
    ClientHandle(ConnectionType conn_type, FileDescriptor socket, event_base* ebase,
        EventCallbackData& callback_data);

    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;

    ClientHandle(ClientHandle&& other) noexcept = default;
    ClientHandle& operator=(ClientHandle&& other) noexcept = default;
    ~ClientHandle() = default;

    // Applicable to all types of ClientHandle
    auto Handshake() -> tb::error<WriteError>;
    auto Write(const Message& m) -> tb::error<WriteError>;
    void Error(std::string_view errstr);
    void Disconnect(std::string_view reason="Disconnected by server");
    void Disconnect_NoWrite();

    auto Available(std::string_view type) -> bool;

    Stream stream; // Only for UNIX/INTERNET

    std::vector<std::string> unavailable;
    Client* client_ptr = nullptr; // Only for INTERNAL connections
    TimePoint last_error = Clock::now();

    ConnectionType conn_type;
    ClientPreferences preferences;

    bool handshaken = false;
    bool connected = false;
};

class Server
{
public:
    Server() = default;
    Server(const Server& other) = delete;
    ~Server();

    auto UnixServer(std::string_view path="buxtehude_unix") -> tb::error<ListenError>;
    auto IPServer(uint16_t port=DEFAULT_PORT) -> tb::error<ListenError>;
    auto InternalServer() -> tb::error<AllocError>;

    void Close();
private: // For INTERNAL connections only.
    friend Client;
    void Internal_AddClient(Client& cl);
    void Internal_RemoveClient(Client& cl);
    void Internal_ReceiveFrom(Client& cl, const Message& msg);
private:
    using HandleIter = std::vector<ClientHandle>::iterator;

    void Run();
    void Serve(HandleIter client_handle);
    void HandleMessage(ClientHandle& client_handle, Message&& msg);
    void Broadcast_NoLock(const Message& msg);

    // Only if listening sockets are opened
    auto SetupEvents() -> tb::error<AllocError>;
    void Listen();
    void AddConnection(FileDescriptor socket, sa_family_t addr_type);

    // Retrieving clients
    auto GetClientBySocket(int fd) -> HandleIter;
    auto GetClientByPointer(Client* ptr) -> HandleIter;
    auto GetFirstAvailable(std::string_view team, std::string_view type,
        const ClientHandle& exclude) -> HandleIter;

    std::vector<ClientHandle> clients;
    std::vector<std::pair<Client*, Message>> internal_messages;
    std::mutex clients_mutex, internal_mutex;

    std::thread current_thread;
    bool started = false;

    // File descriptors for listening sockets
    FileDescriptor unix_server = INVALID_FILE_DESCRIPTOR;
    FileDescriptor ip_server = INVALID_FILE_DESCRIPTOR;

    std::string unix_path;

    // Libevent internals
    UEventBase ebase;
    UEvconnListener ip_listener, unix_listener;
    UEvent interrupt_event, read_internal_event;

    EventCallbackData callback_data;
};

}
