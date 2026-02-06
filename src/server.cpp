#include "server.hpp"

#include <fmt/core.h>

#include "client.hpp"

#include <tb/tb.h>

#include <ranges>

#include <unistd.h>

namespace buxtehude
{

ClientHandle::ClientHandle(Client& iclient, std::string_view teamname)
    : client_ptr(&iclient), conn_type(ConnectionType::INTERNAL), connected(true)
{
    preferences.teamname = teamname;
}

ClientHandle::ClientHandle(ConnectionType conn_type, FileDescriptor socket,
    event_base* ebase, EventCallbackData& callback_data)
    : conn_type(conn_type)
{
    auto stream_or_err = Stream::FromSocket(socket, ebase, callback_data);
    if (stream_or_err.is_error())
        return;

    stream = std::move(stream_or_err.get_mut_unchecked());
    connected = true;
    if (Handshake().is_error()) Disconnect_NoWrite();
}

// Common ClientHandle functions

tb::error<WriteError> ClientHandle::Handshake()
{
    return Write({
        .type { MSG_HANDSHAKE },
        .content = {
            { "version", CURRENT_VERSION }
        }
    });
}

tb::error<WriteError> ClientHandle::Write(const Message& msg)
{
    if (!connected) return WriteError {};

    if (conn_type == ConnectionType::INTERNAL) {
        client_ptr->Internal_Receive(msg);
        return tb::ok;
    }

    stream.WriteMessage(preferences.format, msg).ignore_error();

    return tb::ok;
}

void ClientHandle::Error(std::string_view errstr)
{
    using Seconds = std::chrono::duration<double>;
    if (Clock::now() - last_error < Seconds { 1 })
        return;
    last_error = Clock::now();
    bool success = Write({ .type { MSG_ERROR }, .content = errstr }).is_ok();
    if (!handshaken || !success) Disconnect("Failed handshake");
}

void ClientHandle::Disconnect(std::string_view reason)
{
    if (!connected) return;
    Write({
        .type { MSG_DISCONNECT },
        .content = {
            { "reason", reason },
            { "who", MSG_YOU }
        }
    }).ignore_error();
    Disconnect_NoWrite();
}

void ClientHandle::Disconnect_NoWrite()
{
    if (!connected) return;
    if (conn_type == ConnectionType::UNIX || conn_type == ConnectionType::INTERNET) {
        stream.Close();
    } else if (conn_type == ConnectionType::INTERNAL) {
        client_ptr->Internal_Disconnect();
    }
    logger(LogLevel::DEBUG, fmt::format("Disconnecting client {}",
        preferences.teamname));
    connected = false;
}

bool ClientHandle::Available(std::string_view type)
{
    return std::ranges::find(unavailable, type) == unavailable.end();
}

// ClientHandle functions specific to stream-based connections

// Server
// Server constructors & destructor

Server::~Server()
{
    Close();
}

// Listening socket setup

tb::error<ListenError> Server::UnixServer(std::string_view path)
{
    if (SetupEvents().is_error())
        return ListenError { ListenError::LIBEVENT_ERROR };

    sockaddr_un addr;
    addr.sun_family = PF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = '\0';

    unix_path = addr.sun_path;

    unix_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data, LEV_OPT_CLOSE_ON_FREE,
                                -1, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
    );

    if (!unix_listener) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to listen for UNIX domain connections at {}: {}",
                path, strerror(errno)));
        unix_server = -1;
        return ListenError { ListenError::BIND_ERROR, errno };
    }

    unix_server = evconnlistener_get_fd(unix_listener.get());

    Run();
    logger(LogLevel::DEBUG, fmt::format("Listening on file {}", path));

    return tb::ok;
}

tb::error<ListenError> Server::IPServer(uint16_t port)
{
    if (SetupEvents().is_error())
        return ListenError { ListenError::LIBEVENT_ERROR };

    sockaddr_in addr = {0};
    addr.sin_family = PF_INET;
    addr.sin_port = htons(port);

    if (INADDR_ANY) addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Passing -1 as the backlog allows libevent to try select an optimal backlog number.
    ip_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data,
                                LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
    );

    if (!ip_listener) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to listen for internet domain connections on port {}: {}",
                port, strerror(errno)));
        ip_server = -1;
        return ListenError { ListenError::BIND_ERROR, errno };
    }

    ip_server = evconnlistener_get_fd(ip_listener.get());

    Run();
    logger(LogLevel::DEBUG, fmt::format("Listening on port {}", port));

    return tb::ok;
}

tb::error<AllocError> Server::InternalServer()
{
    if (SetupEvents().is_error()) return AllocError {};

    Run();
    return tb::ok;
}

// Server initialisation & threaded logic

void Server::Run()
{
    if (started) return;
    started = true;

    if (current_thread.joinable()) {
        event_active(interrupt_event.get(), 0, 0);
        current_thread.join();
    }

    current_thread = std::thread(&Server::Listen, this);
}

void Server::Close()
{
    logger(LogLevel::DEBUG, "Shutting down server");
    if (current_thread.joinable()) {
        event_active(interrupt_event.get(), 0, 0);
        current_thread.join();
    }

    for (ClientHandle& handle : clients) {
        handle.Disconnect("Shutting down server");
    }

    if (unix_listener)
        unlink(unix_path.c_str());

    started = false;
}

void Server::Broadcast_NoLock(const Message& m)
{
    for (ClientHandle& handle : clients) {
        if (handle.Write(m).is_error()) handle.Disconnect_NoWrite();
    }
}

// Server connection management
// INTERNAL only functions

void Server::Internal_AddClient(Client& cl)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    auto& handle = clients.emplace_back(cl, cl.preferences.teamname);

    if (handle.Handshake().is_error()) handle.Disconnect_NoWrite();
}

void Server::Internal_RemoveClient(Client& to_remove)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    std::erase_if(clients, [&to_remove] (ClientHandle& handle) {
        return handle.client_ptr == &to_remove;
    });

    Broadcast_NoLock({
        .type { MSG_DISCONNECT },
        .content = {
            { "who", to_remove.preferences.teamname }
        }
    });
}

void Server::Internal_ReceiveFrom(Client& cl, const Message& msg)
{
    std::lock_guard<std::mutex> guard(internal_mutex);
    internal_messages.emplace_back(&cl, msg);
    event_active(read_internal_event.get(), 0, 0);
}

// Reading from socket-based clients

void Server::Serve(HandleIter client_handle)
{
    client_handle->stream.ReadMessage()
    .if_ok_mut([this, client_handle] (Message& message) {
        HandleMessage(*client_handle, std::move(message));
    }).if_err([client_handle] (StreamError error) {
        if (error.type == StreamError::IO_ERROR
            && error.io_error.type == IOError::STREAM_CLOSED) {
            client_handle->Disconnect();
        }
    });

    if (!client_handle->connected) {
        Broadcast_NoLock({
            .type { MSG_DISCONNECT },
            .content = {
                { "who", client_handle->preferences.teamname }
            }
        });

        clients.erase(client_handle);
    }
}

void Server::HandleMessage(ClientHandle& client_handle, Message&& msg)
{
    // Types of the JSON values are validated in checks
    if (!client_handle.handshaken) {
        if (msg.type != MSG_HANDSHAKE ||
            !ValidateJSON(msg.content, VALIDATE_HANDSHAKE_SERVERSIDE)) {
            client_handle.Disconnect("Failed handshake");
            return;
        }

        client_handle.preferences.teamname = msg.content["teamname"];
        client_handle.preferences.format = msg.content["format"];
        client_handle.handshaken = true;
        return;
    }

    if (msg.type == MSG_AVAILABLE) {
        if (!ValidateJSON(msg.content, VALIDATE_AVAILABLE)) {
            client_handle.Error("Incorrect format for $$available message");
            return;
        }
        std::string type = msg.content["type"];
        bool available = msg.content["available"];
        auto iter = std::ranges::find(client_handle.unavailable, type);
        if (available) {
            if (iter != client_handle.unavailable.end())
                client_handle.unavailable.erase(iter);
        } else {
            if (iter == client_handle.unavailable.end())
                client_handle.unavailable.emplace_back(type);
        }
    }

    if (msg.dest.empty()) return;

    msg.src = client_handle.preferences.teamname;
    if (msg.only_first) {
        HandleIter destination = GetFirstAvailable(msg.dest, msg.type, client_handle);
        if (destination != clients.end()) {
            if (destination->Write(msg).is_error()) destination->Disconnect_NoWrite();
        }
        return;
    }

    auto recipients = clients | std::views::filter([&msg] (ClientHandle& handle) {
        return handle.preferences.teamname == msg.dest || msg.dest == MSG_ALL;
    });

    for (ClientHandle& destination : recipients) {
        if (&destination == &client_handle) continue;
        if (destination.Write(msg).is_error()) destination.Disconnect_NoWrite();
    }
}

// Libevent setup

tb::error<AllocError> Server::SetupEvents()
{
    if (ebase) return tb::ok;

    ebase = make<UEventBase>(event_base_new());
    callback_data.event_base = ebase.get();

    interrupt_event = make<UEvent>(
        event_new(ebase.get(), -1, EV_PERSIST, callbacks::LoopInterruptCallback,
                  &callback_data)
    );

    read_internal_event = make<UEvent>(
        event_new(ebase.get(), -1, 0, callbacks::InternalReadCallback, &callback_data)
    );

    if (!ebase || !interrupt_event || !read_internal_event) {
        logger(LogLevel::WARNING, "Failed to allocate one or more libevent structures");
        return AllocError {};
    }

    return tb::ok;
}

void Server::Listen()
{
    while (event_base_loop(ebase.get(), EVLOOP_NO_EXIT_ON_EMPTY) == 0) {
        switch (callback_data.type) {
        case EventType::NEW_CONNECTION: {
            std::lock_guard<std::mutex> guard(clients_mutex);
            AddConnection(callback_data.socket, callback_data.address.sa_family);
            break;
        }
        case EventType::READ_READY: {
            std::lock_guard<std::mutex> guard(clients_mutex);
            Server::HandleIter iter = GetClientBySocket(callback_data.socket);
            if (iter == clients.end()) break;
            Serve(iter);

            break;
        }
        case EventType::TIMEOUT: {
            Server::HandleIter iter = GetClientBySocket(callback_data.socket);
            if (iter == clients.end()) break;

            if (!iter->handshaken) iter->Disconnect("Failed handshake");
            break;
        }
        case EventType::INTERNAL_READ_READY: {
            event_del(read_internal_event.get());
            std::vector<std::pair<Client*, Message>> messages;
            {
                std::lock_guard<std::mutex> guard(internal_mutex);
                messages = std::move(internal_messages);
            }
            std::lock_guard<std::mutex> guard(clients_mutex);
            for (auto& [client_ptr, message] : messages) {
                Server::HandleIter iter = GetClientByPointer(client_ptr);
                if (iter == clients.end()) continue;
                HandleMessage(*iter, std::move(message));
            }
            break;
        }
        case EventType::INTERRUPT:
            return;
        case EventType::WRITE_READY:
            std::lock_guard<std::mutex> guard(clients_mutex);
            Server::HandleIter iter = GetClientBySocket(callback_data.socket);
            if (iter == clients.end()) break;

            iter->stream.Flush().ignore_error();
            break;
        }
    }
}

void Server::AddConnection(int fd, sa_family_t addr_family)
{
    ConnectionType conn_type;
    std::string_view debug_string;

    switch (addr_family) {
    case AF_LOCAL:
        conn_type = ConnectionType::UNIX;
        evconnlistener_enable(unix_listener.get());
        debug_string = "UNIX";
        break;
    case AF_INET:
    default:
        conn_type = ConnectionType::INTERNET;
        evconnlistener_enable(ip_listener.get());
        debug_string = "internet";
        break;
    }

    clients.emplace_back(conn_type, fd, ebase.get(), callback_data);

    logger(LogLevel::DEBUG,
        fmt::format("New client connected on {} domain, fd = {}", debug_string, fd));
}

// ClientHandle iteration

auto Server::GetClientBySocket(int fd) -> HandleIter
{
    auto iter = std::ranges::find_if(clients,
        [fd] (ClientHandle& handle) {
            return handle.stream.GetSocket() == fd;
        }
    );

    if (iter == clients.end())
        logger(LogLevel::WARNING,
            fmt::format("No client with file descriptor {} found", fd));

    return iter;
}

auto Server::GetClientByPointer(Client* ptr) -> HandleIter
{
    auto iter = std::ranges::find_if(clients,
        [ptr] (ClientHandle& handle) {
            return handle.client_ptr == ptr;
        }
    );

    if (iter == clients.end())
        logger(LogLevel::WARNING,
            fmt::format("No client with pointer {} found",
                        static_cast<void*>(ptr)));

    return iter;
}

auto Server::GetFirstAvailable(std::string_view team, std::string_view type,
                               const ClientHandle& exclude) -> HandleIter
{
    HandleIter result = clients.end();

    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if ((it->preferences.teamname == team || team == MSG_ALL) && &(*it) != &exclude)
            result = it;
        else continue;
        if (result->Available(type)) return result;
    }

    return result;
}

}
