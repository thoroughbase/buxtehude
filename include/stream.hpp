#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>

#include <sys/socket.h>
#include <sys/types.h>

#include <tb/tb.h>

#include <event2/event.h>

#include "core.hpp"

namespace buxtehude
{

using Clock = std::chrono::high_resolution_clock;
using FileDescriptor = int;
using TimePoint = std::chrono::time_point<Clock>;
using UFile = std::unique_ptr<FILE, tb::deleter<fclose>>;

constexpr FileDescriptor INVALID_FILE_DESCRIPTOR = -1;

enum class ReadState
{
    AWAITING_MESSAGE_FORMAT, AWAITING_MESSAGE_LENGTH, AWAITING_MESSAGE_DATA
};

struct EventCallbackData
{
    sockaddr address;
    size_t address_length;
    event_base* event_base;
    FileDescriptor socket;
    EventType type;
};

struct IOError
{
    enum Type
    {
        STREAM_CLOSED, BUFFER_FULL, BUFFER_EMPTY, FILE_ERROR
    };

    Type type;
    ErrnoCode code = ERRNO_NO_ERROR;
};

struct StreamError
{
    enum Type
    {
        LIBEVENT_ERROR, IO_ERROR, INVALID_MESSAGE_TYPE, INVALID_MESSAGE_LENGTH,
        PARSE_ERROR
    };

    constexpr auto What() -> std::string_view
    {
        switch (type) {
        case LIBEVENT_ERROR: return "failed to initialise libevent";
        case IO_ERROR: return "I/O error";
        case INVALID_MESSAGE_TYPE: return "invalid message type";
        case INVALID_MESSAGE_LENGTH: return "invalid message length";
        case PARSE_ERROR: return "parse error";
        }
    }

    Type type;
    IOError io_error;
};

struct IOResult
{
    size_t bytes;
    tb::error<IOError> error;
};

class ByteBuffer
{
public:
    ByteBuffer(size_t size);

    auto WriteFromStream(FILE* stream, size_t bytes) -> IOResult;
    auto WriteFromMemory(tb::contiguous_byte_range auto const& source) -> IOResult;

    template<typename T> requires std::is_scalar_v<T>
    auto WriteFromMemory(T object) -> IOResult;
    auto ReadIntoStream(FILE* stream, size_t bytes) -> IOResult;

    template<typename T> requires std::is_scalar_v<T>
    auto ReadIntoMemory(T& object) -> IOResult;

    void Reset();
    auto BytesToRead() const -> size_t;
    auto ReadView() const -> std::span<uint8_t>;

private:
    tb::dynamically_allocated_array<uint8_t, std::dynamic_extent> data_;
    size_t write_position_ = 0;
    size_t read_position_ = 0;
};

class Stream
{
public:
    constexpr static size_t BUFFER_SIZE = sizeof(MessageFormat) + sizeof(uint32_t)
                                        + MAX_MESSAGE_LENGTH;
    Stream() = default;

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    Stream(Stream&&) = default;
    Stream& operator=(Stream&&) = default;

    static auto FromSocket(FileDescriptor socket, event_base* ebase,
        EventCallbackData& callback_data)
    -> tb::result<Stream, StreamError>;

    auto WriteMessage(MessageFormat format, const Message& message)
    -> tb::error<IOError>;
    auto ReadMessage() -> tb::result<Message, StreamError>;
    auto Flush() -> tb::error<IOError>;

    auto GetSocket() const -> FileDescriptor;
    void Close();

private:
    ByteBuffer read_buffer_ { BUFFER_SIZE };
    ByteBuffer write_buffer_ { BUFFER_SIZE };
    UFile stream_handle_;
    UEvent read_event_, write_event_;
    ReadState read_state_ = ReadState::AWAITING_MESSAGE_FORMAT;
    FileDescriptor socket_ = INVALID_FILE_DESCRIPTOR;
    uint32_t expected_length_;
    MessageFormat expected_format_;
};

}
