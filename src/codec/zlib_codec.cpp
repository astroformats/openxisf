// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// zlib (spec §10.6.3): the zlib format of RFC 1950 over deflate. zlib counts bytes in 32-bit integers, so the streaming
// API is fed in pieces, and no block is too large for it.

#include <openxisf/error.h>

#include "codec/codecs.h"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace openxisf::detail {

namespace {

static_assert(zlib_max_piece == std::numeric_limits<uInt>::max());

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

[[noreturn]] void throw_failure(int result, const z_stream& stream)
{
    if (result == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    throw unsupported_error(errc::codec_failure, "zlib failed with error " + std::to_string(result) +
                                                     (stream.msg != nullptr ? ": " + std::string(stream.msg) : ""));
}

Bytef* bytes_of(std::byte* data) noexcept
{
    return reinterpret_cast<Bytef*>(data);
}

// zlib takes its input through a pointer to non-const bytes, which it does not change.
Bytef* bytes_of(const std::byte* data) noexcept
{
    return const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
}

// The part of size bytes that zlib gets in one call.
uInt piece(std::size_t size, std::size_t max_piece) noexcept
{
    return static_cast<uInt>(std::min(size, std::clamp<std::size_t>(max_piece, 1, zlib_max_piece)));
}

// The input or output of a stream, given to zlib a piece at a time.
template <typename Byte> class buffer
{
public:
    buffer(std::span<Byte> data, std::size_t max_piece) noexcept : data_(data), max_piece_(max_piece) {}

    // Gives zlib the next piece when it has used up the previous one.
    void refill(Bytef*& next, uInt& available) noexcept
    {
        if (available == 0 && given_ < data_.size()) {
            available = piece(data_.size() - given_, max_piece_);
            next = bytes_of(data_.data() + given_);
            given_ += available;
        }
    }

    // The bytes that zlib has not reached, given the counter of the current piece.
    [[nodiscard]] std::size_t left(uInt available) const noexcept
    {
        return data_.size() - given_ + available;
    }

private:
    std::span<Byte> data_;
    std::size_t max_piece_;
    std::size_t given_ = 0;
};

class inflater
{
public:
    inflater()
    {
        const int result = inflateInit(&stream_);
        if (result != Z_OK) {
            throw_failure(result, stream_);
        }
    }
    inflater(const inflater&) = delete;
    inflater& operator=(const inflater&) = delete;
    ~inflater()
    {
        (void)inflateEnd(&stream_);
    }

    z_stream& stream() noexcept
    {
        return stream_;
    }

private:
    z_stream stream_{};
};

class deflater
{
public:
    explicit deflater(int level)
    {
        const int result = deflateInit(&stream_, level);
        if (result != Z_OK) {
            throw_failure(result, stream_);
        }
    }
    deflater(const deflater&) = delete;
    deflater& operator=(const deflater&) = delete;
    ~deflater()
    {
        (void)deflateEnd(&stream_);
    }

    z_stream& stream() noexcept
    {
        return stream_;
    }

private:
    z_stream stream_{};
};

} // namespace

void zlib_decompress(std::span<const std::byte> input, std::span<std::byte> output, std::size_t max_piece)
{
    inflater state;
    z_stream& stream = state.stream();
    buffer<const std::byte> in(input, max_piece);
    buffer<std::byte> out(output, max_piece);
    // Once output is full, zlib gets this byte as its output, so that a stream with more data shows.
    std::byte probe{};
    bool full = false;

    for (;;) {
        in.refill(stream.next_in, stream.avail_in);
        if (!full) {
            out.refill(stream.next_out, stream.avail_out);
            if (stream.avail_out == 0) {
                full = true;
                stream.next_out = bytes_of(&probe);
                stream.avail_out = 1;
            }
        }
        const int result = inflate(&stream, Z_NO_FLUSH);
        if (full && stream.avail_out == 0) {
            throw_corrupt("the zlib data decode to more than " + std::to_string(output.size()) + " bytes");
        }
        if (result == Z_STREAM_END) {
            break;
        }
        switch (result) {
        case Z_OK:
            continue;
        case Z_BUF_ERROR:
            // No progress: the stream needs more input than there is.
            throw_corrupt("the zlib data end before the end of the stream");
        case Z_NEED_DICT:
            throw_corrupt("the zlib data need a preset dictionary");
        case Z_DATA_ERROR:
            throw_corrupt("the zlib data are corrupt" + (stream.msg != nullptr ? ": " + std::string(stream.msg) : ""));
        default:
            throw_failure(result, stream);
        }
    }

    if (const std::size_t missing = full ? 0 : out.left(stream.avail_out); missing != 0) {
        throw_corrupt("the zlib data decode to " + std::to_string(output.size() - missing) + " bytes, not " +
                      std::to_string(output.size()));
    }
    if (const std::size_t extra = in.left(stream.avail_in); extra != 0) {
        throw_corrupt("the zlib stream is followed by " + std::to_string(extra) + " more bytes");
    }
}

struct zlib_encoder::state
{
    deflater stream;
    // Whether the stream has compressed an input, and needs deflateReset() before the next.
    bool used = false;

    explicit state(int level) : stream(level) {}
};

zlib_encoder::zlib_encoder(int level, std::size_t max_piece)
    : state_(std::make_unique<state>(level)), max_piece_(max_piece)
{}

zlib_encoder::~zlib_encoder() = default;

void zlib_encoder::compress(std::span<const std::byte> input, std::vector<std::byte>& output)
{
    z_stream& stream = state_->stream.stream();
    if (state_->used) {
        // Keeps the level and the memory of the stream, and starts a new one.
        const int result = deflateReset(&stream);
        if (result != Z_OK) {
            throw_failure(result, stream);
        }
    }
    state_->used = true;
    // A compression that threw may have left input in the stream.
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    buffer<const std::byte> in(input, max_piece_);
    const std::size_t start = output.size();
    // More than deflate needs for data it cannot compress, which zlib bounds at a little more than the data
    // (deflateBound()); the output still grows if another implementation of zlib ever needs more.
    output.resize(start + input.size() + (input.size() / 8) + 64);
    std::size_t written = 0;

    for (;;) {
        in.refill(stream.next_in, stream.avail_in);
        if (written == output.size() - start) {
            output.resize(output.size() + (output.size() - start) / 2);
        }
        const std::size_t room = output.size() - start - written;
        stream.next_out = bytes_of(output.data() + start + written);
        stream.avail_out = piece(room, max_piece_);
        const uInt offered = stream.avail_out;
        const int flush = in.left(stream.avail_in) == stream.avail_in ? Z_FINISH : Z_NO_FLUSH;
        const int result = deflate(&stream, flush);
        written += offered - stream.avail_out;
        if (result == Z_STREAM_END) {
            break;
        }
        if (result != Z_OK && result != Z_BUF_ERROR) {
            throw_failure(result, stream);
        }
    }
    output.resize(start + written);
}

void zlib_compress(std::span<const std::byte> input, int level, std::vector<std::byte>& output, std::size_t max_piece)
{
    zlib_encoder(level, max_piece).compress(input, output);
}

} // namespace openxisf::detail
