#include "stream.hpp"

#include <algorithm>

namespace buxtehude
{

ByteBuffer::ByteBuffer(size_t size) : data_(size) {}

auto ByteBuffer::WriteFromStream(FILE* stream, size_t bytes) -> tb::error<IOError>
{
    if (write_position_ + bytes > data_.view().size())
        return IOError { IOError::BUFFER_FULL };

    size_t bytes_read = fread(data_.view().data() + write_position_, 1, bytes, stream);
    write_position_ += bytes_read;

    if (bytes_read < bytes) {
        if (feof(stream))
            return IOError { IOError::STREAM_CLOSED, errno };
        return IOError { IOError::FILE_ERROR, errno };
    }

    return tb::ok;
}

auto ByteBuffer::WriteFromMemory(tb::contiguous_byte_range auto const& source)
-> tb::error<IOError>
{
    if (write_position_ + std::size(source) > data_.view().size())
        return IOError { IOError::BUFFER_FULL };

    memcpy(data_.view().data() + write_position_, std::data(source), std::size(source));
    write_position_ += source.size();

    return tb::ok;
}

template<typename T> requires std::is_scalar_v<T>
auto ByteBuffer::WriteFromMemory(T object) -> tb::error<IOError>
{
    if (write_position_ + sizeof(T) > data_.view().size())
        return IOError { IOError::BUFFER_FULL };

    memcpy(data_.view().data() + write_position_, &object, sizeof(T));
    write_position_ += sizeof(T);

    return tb::ok;
}

auto ByteBuffer::ReadIntoStream(FILE* stream, size_t bytes) -> tb::error<IOError>
{
    if (read_position_ == write_position_)
        return IOError { IOError::BUFFER_EMPTY };

    bytes = std::min(BytesToRead(), bytes);

    size_t bytes_written = fwrite(data_.view().data() + read_position_, 1, bytes, stream);
    read_position_ += bytes_written;

    if (bytes_written < bytes) {
        if (feof(stream))
            return IOError { IOError::STREAM_CLOSED, errno };
        return IOError { IOError::FILE_ERROR, errno };
    }

    return tb::ok;
}

template<typename T> requires std::is_scalar_v<T>
auto ByteBuffer::ReadIntoMemory(T& object) -> tb::error<IOError>
{
    if (BytesToRead() < sizeof(T))
        return IOError { IOError::BUFFER_EMPTY };

    memcpy(&object, data_.view().data() + read_position_, sizeof(T));
    read_position_ += sizeof(T);

    return tb::ok;
}

void ByteBuffer::Reset()
{
    write_position_ = 0;
    read_position_ = 0;
}

auto ByteBuffer::BytesToRead() const -> size_t
{
    return write_position_ - read_position_;
}

auto ByteBuffer::ReadView() const -> std::span<uint8_t>
{
    return std::span<uint8_t> { data_.view().data() + read_position_, BytesToRead() };
}

auto Stream::FromSocket(FileDescriptor socket, event_base* ebase,
    EventCallbackData& callback_data)
-> tb::result<Stream, StreamError>
{
    Stream stream;
    stream.socket_ = socket;

    stream.stream_handle_.reset(fdopen(socket, "r+"));
    if (stream.stream_handle_.get() == nullptr)
        return StreamError {
            StreamError::IO_ERROR,
            IOError { IOError::FILE_ERROR, errno }
        };

    setvbuf(stream.stream_handle_.get(), nullptr, _IONBF, 0);
    int flags = fcntl(stream.socket_, F_GETFL);
    fcntl(stream.socket_, F_SETFL, flags | O_NONBLOCK);

    stream.read_event_.reset(
        event_new(
            ebase, stream.socket_, EV_PERSIST | EV_READ,
            callbacks::ReadWriteCallback, static_cast<void*>(&callback_data)
        )
    );

    stream.write_event_.reset(
        event_new(
            ebase, stream.socket_, EV_WRITE,
            callbacks::ReadWriteCallback, static_cast<void*>(&callback_data)
        )
    );

    if (stream.read_event_.get() == nullptr || stream.write_event_.get() == nullptr)
        return StreamError { StreamError::LIBEVENT_ERROR };

    event_add(stream.read_event_.get(), &callbacks::DEFAULT_TIMEOUT);

    return stream;
}

auto Stream::WriteMessage(MessageFormat format, const Message& message)
-> tb::error<IOError>
{
    clearerr(stream_handle_.get());

    std::ignore = write_buffer_.WriteFromMemory(format);
    switch (format) {
    case MessageFormat::JSON: {
        json object = message;
        std::string serialised = object.dump();

        std::ignore = write_buffer_.WriteFromMemory<uint32_t>(serialised.size());
        if (auto io_error = write_buffer_.WriteFromMemory(serialised);
            io_error.is_error())
            return io_error;
        break;
    }
    case MessageFormat::MSGPACK: {
        std::vector<uint8_t> serialised = json::to_msgpack(message);

        std::ignore = write_buffer_.WriteFromMemory<uint32_t>(serialised.size());
        if (auto io_error = write_buffer_.WriteFromMemory(serialised);
            io_error.is_error())
            return io_error;
        break;
    }
    }

    return Flush();
}

auto Stream::ReadMessage() -> tb::result<Message, StreamError>
{
    auto reset_stream_state = [this] () {
        read_buffer_.Reset();
        read_state_ = ReadState::AWAITING_MESSAGE_FORMAT;
    };

    clearerr(stream_handle_.get());

    if (read_state_ == ReadState::AWAITING_MESSAGE_FORMAT) {
        auto io_error = read_buffer_.WriteFromStream(
            stream_handle_.get(),
            sizeof(MessageFormat) - read_buffer_.BytesToRead()
        );
        if (io_error.is_error())
            return StreamError { StreamError::IO_ERROR, io_error.get_error() };

        read_buffer_.ReadIntoMemory(expected_format_).ignore_error();
        if (expected_format_ != MessageFormat::JSON
            && expected_format_ != MessageFormat::MSGPACK) {
            reset_stream_state();
            return StreamError { StreamError::INVALID_MESSAGE_TYPE };
        }

        read_state_ = ReadState::AWAITING_MESSAGE_LENGTH;
    }

    if (read_state_ == ReadState::AWAITING_MESSAGE_LENGTH) {
        auto io_error = read_buffer_.WriteFromStream(
            stream_handle_.get(),
            sizeof(uint32_t) - read_buffer_.BytesToRead()
        );
        if (io_error.is_error())
            return StreamError { StreamError::IO_ERROR, io_error.get_error() };

        read_buffer_.ReadIntoMemory(expected_length_).ignore_error();
        if (expected_length_ > MAX_MESSAGE_LENGTH) {
            reset_stream_state();
            return StreamError { StreamError::INVALID_MESSAGE_LENGTH };
        }

        read_state_ = ReadState::AWAITING_MESSAGE_DATA;
    }

    json object;
    if (read_state_ == ReadState::AWAITING_MESSAGE_DATA) {
        auto io_error = read_buffer_.WriteFromStream(
            stream_handle_.get(),
            expected_length_ - read_buffer_.BytesToRead()
        );
        if (io_error.is_error())
            return StreamError { StreamError::IO_ERROR, io_error.get_error() };

        switch (expected_format_) {
        case MessageFormat::JSON:
            object = json::parse(read_buffer_.ReadView(), nullptr, false);
            break;
        case MessageFormat::MSGPACK:
            object = json::from_msgpack(read_buffer_.ReadView(), true, false);
            break;
        }

        reset_stream_state();
        if (object.is_discarded())
            return StreamError { StreamError::PARSE_ERROR };
    }
    return object.get<Message>();
}

auto Stream::Flush() -> tb::error<IOError>
{
    clearerr(stream_handle_.get());

    auto io_error = write_buffer_.ReadIntoStream(
        stream_handle_.get(),
        write_buffer_.BytesToRead()
    );

    if (io_error.is_ok()) {
        write_buffer_.Reset();
        return tb::ok;
    }

    if (io_error.get_error().code == EAGAIN) {
        event_add(write_event_.get(), nullptr);
        return tb::ok;
    }

    return io_error;
}

auto Stream::GetSocket() const -> FileDescriptor
{
    return socket_;
}

void Stream::Close()
{
    fclose(stream_handle_.get());
}

}
