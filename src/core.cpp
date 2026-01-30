#include "core.hpp"

#include <fmt/core.h>
#include <event2/thread.h>

#include <signal.h>

#include "stream.hpp"

namespace buxtehude
{

// JSON conversion functions

void to_json(json& j, const Message& msg)
{
    j = { { "type", msg.type }, { "only_first", msg.only_first } };
    if (!msg.dest.empty()) j["dest"] = msg.dest;
    if (!msg.src.empty()) j["src"] = msg.src;
    if (!msg.content.empty()) j["content"] = msg.content;
}

void from_json(const json& j, Message& msg)
{
    if (j.contains("dest")) j["dest"].get_to(msg.dest);
    if (j.contains("src")) j["src"].get_to(msg.src);
    if (j.contains("type")) j["type"].get_to(msg.type);
    if (j.contains("only_first")) j["only_first"].get_to(msg.only_first);
    if (j.contains("content")) j["content"].get_to(msg.content);
}

// Logging & Library initialisation

void DefaultLog(LogLevel l, std::string_view message)
{
    constexpr static std::string_view LEVEL_NAMES[] = {
        "DEBUG", "INFO", "WARNING", "SEVERE"
    };
    fmt::print("[{}] {}\n", LEVEL_NAMES[static_cast<size_t>(l)], message);
}

void Initialise(LogCallback logcb, SignalHandler sigh)
{
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
    evthread_use_pthreads();
#elif EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
    evthread_use_windows_threads();
#else
    static_assert(0,
        "Buxtehude requires Libevent to have been built with thread support");
#endif

    logger = logcb ? logcb : DefaultLog;
    event_set_log_callback([] (int severity, const char* msg) {
        logger(static_cast<LogLevel>(severity), msg);
    });

    // UNIX domain connections being closed sends signal SIGPIPE to the process trying to
    // read from this socket. Not intercepting this signal would kill the process.
    struct sigaction sighandle {};
    sighandle.sa_handler = sigh ? sigh : SIG_IGN;
    sigaction(SIGPIPE, &sighandle, nullptr);
}

namespace callbacks
{

void ConnectionCallback(evconnlistener* listener, evutil_socket_t fd,
    sockaddr* addr, int addr_len, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);

    ecdata->socket = fd;
    ecdata->address = *addr;
    ecdata->address_length = addr_len;
    ecdata->type = EventType::NEW_CONNECTION;

    event_base_loopbreak(ecdata->event_base);

    // If there is a queue of connections, these callbacks will run in succession
    // even with a call to event_base_loopbreak(). Disabling the listener will
    // break out of the listener accept(2) loop. The listener is re-enabled after
    // accepting the connection in Server::AddConnection().
    evconnlistener_disable(listener);
}

void ReadWriteCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);

    ecdata->socket = fd;
    if (what & EV_READ) ecdata->type = EventType::READ_READY;
    else if (what & EV_WRITE) ecdata->type = EventType::WRITE_READY;
    else if (what & EV_TIMEOUT) ecdata->type = EventType::TIMEOUT;

    event_base_loopbreak(ecdata->event_base);
}

void LoopInterruptCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);
    ecdata->type = EventType::INTERRUPT;
    event_base_loopbreak(ecdata->event_base);
}

void InternalReadCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);
    ecdata->type = EventType::INTERNAL_READ_READY;
    event_base_loopbreak(ecdata->event_base);
}

}

}
