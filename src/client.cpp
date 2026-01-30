#include "client.hpp"

#include "server.hpp"
#include "core.hpp"
#include <tb/tb.h>

#include <fmt/core.h>

namespace buxtehude
{

Client::~Client()
{
    Disconnect();

    if (current_thread.joinable()
        && std::this_thread::get_id() != current_thread.get_id()) {
        current_thread.join();
    }
}

Client::Client(const ClientPreferences& preferences) : preferences(preferences) {}

// Connection setup functions

tb::error<ConnectError> Client::IPConnect(std::string_view hostname, uint16_t port)
{
    if (connected) return ConnectError { ConnectErrorType::ALREADY_CONNECTED };

    conn_type = ConnectionType::INTERNET;

    addrinfo* res;
    addrinfo hints {
        .ai_flags = AI_ADDRCONFIG,
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };

    if (int gai_error = getaddrinfo(hostname.data(), nullptr, &hints, &res)) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to connect to address {}: getaddrinfo failed: {}",
                hostname, gai_strerror(gai_error)));
        return ConnectError { ConnectErrorType::GETADDRINFO_ERROR, gai_error };
    }

    tb::scoped_guard addrinfo_guard = [res] () { freeaddrinfo(res); };

    int client_socket = socket(res->ai_family, res->ai_socktype, 0);
    if (client_socket == -1)
        return ConnectError { ConnectErrorType::SOCKET_ERROR, errno };

    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr->sin_port = htons(port);

    if (connect(client_socket, reinterpret_cast<sockaddr*>(addr), sizeof(sockaddr_in))) {
        logger(LogLevel::WARNING, fmt::format("Failed to connect to address {}: {}",
            hostname, strerror(errno)));
        return ConnectError { ConnectErrorType::CONNECT_ERROR, errno };
    }

    connected = true;

    if (SetupEvents(client_socket).is_error())
        return ConnectError { ConnectErrorType::LIBEVENT_ERROR };

    if (Handshake().is_error())
        return ConnectError { ConnectErrorType::WRITE_ERROR };

    StartListening();

    return tb::ok;
}

tb::error<ConnectError> Client::UnixConnect(std::string_view path)
{
    if (connected) return ConnectError { ConnectErrorType::ALREADY_CONNECTED };

    conn_type = ConnectionType::UNIX;

    FileDescriptor client_socket = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (client_socket == INVALID_FILE_DESCRIPTOR)
        return ConnectError { ConnectErrorType::SOCKET_ERROR, errno };

    sockaddr_un addr;
    addr.sun_family = AF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = '\0';

    if (connect(client_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_un))) {
        logger(LogLevel::WARNING, fmt::format("Failed to connect to file {}: {}",
            path, strerror(errno)));
        return ConnectError { ConnectErrorType::CONNECT_ERROR, errno };
    }

    connected = true;

    if (SetupEvents(client_socket).is_error())
        return ConnectError { ConnectErrorType::LIBEVENT_ERROR };

    if (Handshake().is_error())
        return ConnectError { ConnectErrorType::WRITE_ERROR };

    StartListening();

    return tb::ok;
}

tb::error<ConnectError> Client::InternalConnect(Server& server)
{
    if (connected) return ConnectError { ConnectErrorType::ALREADY_CONNECTED };

    conn_type = ConnectionType::INTERNAL;

    server_ptr = &server;
    server.Internal_AddClient(*this);
    connected = true;

    // This can only fail if the server closes between the AddClient call and
    // trying to write for the handshake.
    if (Handshake().is_error()) {
        server_ptr = nullptr;
        return ConnectError { ConnectErrorType::WRITE_ERROR };
    }

    StartListening();

    return tb::ok;
}

// General functions applicable to all types of Client

tb::error<WriteError> Client::Write(const Message& msg)
{
    if (!connected) return WriteError {};

    if (conn_type == ConnectionType::INTERNAL) {
        if (!server_ptr) return WriteError {};
        server_ptr.load()->Internal_ReceiveFrom(*this, msg);
        return tb::ok;
    }

    auto result = stream.WriteMessage(preferences.format, msg);
    if (result.is_error()) {
        IOError error = result.get_error();
        if (error.type == IOError::STREAM_CLOSED) {
            Disconnect();
        }
        return WriteError {};
    }

    return tb::ok;
}

tb::error<WriteError> Client::Handshake()
{
    SetupDefaultHandlers();

    return Write({
        .type { MSG_HANDSHAKE },
        .content = {
            { "format", preferences.format },
            { "teamname", preferences.teamname },
            { "version", CURRENT_VERSION },
        }
    });
}

void Client::SetupDefaultHandlers()
{
    AddHandler(MSG_HANDSHAKE, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_HANDSHAKE_CLIENTSIDE)) {
            logger(LogLevel::WARNING, "Rejected server handshake - disconnecting");
            c.Disconnect();
            return;
        }

        c.EraseHandler(std::string { MSG_HANDSHAKE });
    });

    AddHandler(MSG_ERROR, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_SERVER_MESSAGE)) {
            logger(LogLevel::WARNING, "Erroneous server message");
            return;
        }

        logger(LogLevel::INFO, fmt::format("Error message from server: {}",
               m.content.get<std::string>()));
    });
}

tb::error<WriteError> Client::SetAvailable(std::string_view type, bool available)
{
    return Write({
        .type { MSG_AVAILABLE },
        .content = {
            { "type", type },
            { "available", available }
        }
    });
}

void Client::HandleMessage(const Message& msg)
{
    if (msg.type.empty()) {
        logger(LogLevel::WARNING, "Received message with no type!");
        return;
    }

    if (handlers.contains(msg.type)) handlers[msg.type](*this, msg);
}

// Handlers

void Client::AddHandler(std::string_view type, Handler&& h)
{
    handlers.emplace(type, std::forward<Handler>(h));
}

void Client::SetDisconnectHandler(DisconnectHandler&& h)
{
    disconnect_handler = std::move(h);
}

void Client::EraseHandler(const std::string& type) { handlers.erase(type); }

void Client::ClearHandlers() { handlers.clear(); }

bool Client::Connected() const { return connected; }

void Client::StartListening()
{
    if (conn_type != ConnectionType::INTERNAL) {
        if (current_thread.joinable()) current_thread.join();
        current_thread = std::thread(&Client::Listen, this);
    }
}

void Client::Disconnect()
{
    if (!connected) return;
    connected = false;

    logger(LogLevel::DEBUG, "Disconnecting client");

    if (conn_type != ConnectionType::INTERNAL) {
        event_active(interrupt_event.get(), 0, 0);
    } else if (conn_type == ConnectionType::INTERNAL && server_ptr) {
        server_ptr.load()->Internal_RemoveClient(*this);
    }

    if (disconnect_handler) disconnect_handler(*this);
}

void Client::Internal_Disconnect()
{
    if (!connected) return;
    connected = false;
    server_ptr = nullptr;
    logger(LogLevel::DEBUG, "Disconnecting client");
    if (disconnect_handler) disconnect_handler(*this);
}

// INTERNAL clients only

void Client::Internal_Receive(const Message& msg)
{
    HandleMessage(msg);
}

// Socket-based connections only

tb::error<AllocError> Client::SetupEvents(FileDescriptor socket)
{
    ebase = make<UEventBase>(event_base_new());
    callback_data.event_base = ebase.get();

    interrupt_event = make<UEvent>(
        event_new(ebase.get(), -1, EV_PERSIST,
                  callbacks::LoopInterruptCallback, &callback_data)
    );

    if (!ebase || !interrupt_event) {
        logger(LogLevel::WARNING, "Failed to create one or more libevent structures");
        return AllocError {};
    }

    auto stream_or_err = Stream::FromSocket(socket, ebase.get(), callback_data);
    if (stream_or_err.is_error())
        return AllocError {};

    stream = std::move(stream_or_err.get_mut_unchecked());

    return tb::ok;
}

void Client::Listen()
{
    while (event_base_dispatch(ebase.get()) == 0) {
        switch (callback_data.type) {
        case EventType::READ_READY:
            stream.ReadMessage().if_ok_mut([this] (const Message& msg) {
                HandleMessage(msg);
            }).if_err([this] (StreamError error) {
                if (error.type == StreamError::IO_ERROR
                    && error.io_error.type == IOError::STREAM_CLOSED)
                    Disconnect();
            });
            break;
        case EventType::INTERRUPT:
            return;
        case EventType::WRITE_READY:
            stream.Flush().if_err([this] (IOError error) {
                if (error.type == IOError::STREAM_CLOSED)
                    Disconnect();
            });
            break;
        default:
            break;
        }
    }
}

}
