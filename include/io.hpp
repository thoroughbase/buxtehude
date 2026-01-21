#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <tuple>
#include <string_view>
#include <ranges>

#include <tb/tb.h>

namespace buxtehude
{

struct Field;
class Stream;

using Callback = std::function<void(Stream&, Field&)>;
using FieldIterator = std::list<Field>::iterator;

enum class StreamStatus
{
    REACHED_EOF, OKAY
};

struct Field
{
    std::vector<uint8_t> data;
    size_t length;
    FieldIterator self_iterator;
    Callback cb;

    Field(size_t length) : length(length) { data.reserve(length); }

    Field(Field&&) noexcept = default;
    Field& operator=(Field&&) noexcept = default;

    template<typename T>
    T Get() { return *reinterpret_cast<T*>(data.data()); }

    template<typename T>
    std::pair<const T*, size_t> GetPtr()
    {
        return { reinterpret_cast<const T*>(data.data()), length };
    }

    std::string_view GetView()
    {
        return { reinterpret_cast<const char*>(data.data()), length };
    }

    Field& operator[](int offset);
};

class Stream
{
public:
    Stream() = default;
    Stream(FILE* file);

    Stream(Stream&&) noexcept = default;
    Stream& operator=(Stream&&) noexcept = default;

    Stream(const Stream&) = delete;

    template<typename T=void>
    Stream& Await(size_t len=sizeof(T))
    {
        FieldIterator iter = fields.emplace(fields.end(), len);
        Field& new_field = *iter;
        new_field.self_iterator = iter;

        if (deleted.empty()) {
            new_field.data.reserve(len);
        } else {
            auto reusable = std::ranges::find_if(deleted,
                [len] (const Field& f) {
                    return f.data.capacity() >= len;
                }
            );

            if (reusable == deleted.end()) {
                new_field.data.reserve(len);
                deleted.erase(deleted.begin());
            } else {
                new_field.data = std::move(reusable->data);
                deleted.erase(reusable);
            }
        }

        return *this;
    }

    Stream& Then(Callback&& cb);
    void Finally(Callback&& cb);
    FieldIterator Delete(Field& f);

    bool Read();
    bool Done();
    StreamStatus Status();
    void Reset();
    void Rewind(int offset);
    void ClearFields();

    Field& operator[](int offset);

    template<std::ranges::contiguous_range ByteRange>
        requires (sizeof(std::ranges::range_value_t<ByteRange>) == 1
        && std::integral<std::ranges::range_value_t<ByteRange>>)
    tb::error<int> TryWrite(ByteRange&& src)
    {
        if (!output_buffer.empty()) {
            output_buffer.insert(output_buffer.end(), std::begin(src), std::end(src));

            size_t bytes_to_write = output_buffer.size();
            size_t bytes_written = fwrite(output_buffer.data(), 1, bytes_to_write, file);

            output_buffer.erase(output_buffer.begin(),
                output_buffer.begin() + bytes_written);
        } else {
            size_t bytes_written = fwrite(std::data(src), 1, std::size(src), file);

            output_buffer.insert(output_buffer.end(),
                std::begin(src) + bytes_written,
                std::end(src));
        }

        if (output_buffer.size())
            return errno;
        else
            return tb::ok;
    }

    tb::error<int> Flush()
    {
        size_t bytes_written
            = fwrite(output_buffer.data(), 1, output_buffer.size(), file);

        output_buffer.erase(output_buffer.begin(), output_buffer.begin() + bytes_written);
        if (output_buffer.size())
            return errno;
        else
            return tb::ok;
    }

    FILE* file = nullptr;
private:
    Callback finally;

    std::list<Field> fields, deleted;
    std::vector<uint8_t> output_buffer;
    FieldIterator current = fields.end();
    size_t data_offset = 0;
    StreamStatus status = StreamStatus::OKAY;
    bool done = false;
    bool is_at_valid_field = false;
};

}
