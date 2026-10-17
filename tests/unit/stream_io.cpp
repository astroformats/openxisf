// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/stream_io.h>

#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <ios>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::io_error;
using openxisf::usage_error;
using openxisf::test::bytes;
using openxisf::test::c_file;
using openxisf::test::injected_fault;
using openxisf::test::read_file;
using openxisf::test::temp_directory;
using openxisf::test::text;
using openxisf::test::throws;
using openxisf::test::write_file;

// A buffer that reads only the first half of what is asked for, like a stream that ends early.
class short_buffer : public std::stringbuf
{
public:
    explicit short_buffer(const std::string& content) : std::stringbuf(content) {}

protected:
    std::streamsize xsgetn(char* destination, std::streamsize count) override
    {
        return std::stringbuf::xsgetn(destination, count / 2);
    }
};

// A buffer whose reads and writes throw, like a failing device. The stream catches the exception and sets badbit.
class failing_buffer : public std::stringbuf
{
public:
    failing_buffer() = default;
    explicit failing_buffer(const std::string& content) : std::stringbuf(content) {}

protected:
    std::streamsize xsgetn(char* /*destination*/, std::streamsize /*count*/) override
    {
        throw injected_fault();
    }

    std::streamsize xsputn(const char* /*source*/, std::streamsize /*count*/) override
    {
        throw injected_fault();
    }

    int sync() override
    {
        return -1;
    }
};

// A buffer that writes until it is told to fail, like a medium that fails after the first bytes.
class failing_later_buffer : public std::stringbuf
{
public:
    void fail() noexcept
    {
        failing_ = true;
    }

protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override
    {
        if (failing_) {
            throw injected_fault();
        }
        return std::stringbuf::xsputn(source, count);
    }

private:
    bool failing_ = false;
};

// Makes the descriptor of stream that of another stream, from now on, as dup2() does: what stream has buffered goes
// there when it is flushed.
void redirect(std::FILE* stream, std::FILE* other)
{
#if defined(_WIN32)
    const int moved = _dup2(_fileno(other), _fileno(stream));
#else
    const int moved = ::dup2(::fileno(other), ::fileno(stream));
#endif
    if (moved < 0) {
        throw std::runtime_error("cannot redirect a stream");
    }
}

// A buffer whose first read throws, like a device that fails once.
class failing_once_buffer : public std::stringbuf
{
public:
    explicit failing_once_buffer(const std::string& content) : std::stringbuf(content) {}

protected:
    std::streamsize xsgetn(char* destination, std::streamsize count) override
    {
        if (!failed_) {
            failed_ = true;
            throw injected_fault();
        }
        return std::stringbuf::xsgetn(destination, count);
    }

private:
    bool failed_ = false;
};

// The two ends of a pipe, as binary C streams that cannot seek. Both are closed on destruction.
class pipe_streams
{
public:
    pipe_streams()
    {
        std::array<int, 2> ends{};
#if defined(_WIN32)
        const int made = _pipe(ends.data(), 4096, _O_BINARY);
#else
        const int made = ::pipe(ends.data());
#endif
        if (made != 0) {
            throw std::runtime_error("cannot create a pipe");
        }
        // The destructor does not run when the constructor throws, so what was opened is closed here.
        read_ = open(ends[0], "rb");
        if (read_ == nullptr) {
            close_descriptor(ends[0]);
            close_descriptor(ends[1]);
            throw std::runtime_error("cannot open a stream on a pipe");
        }
        write_ = open(ends[1], "wb");
        if (write_ == nullptr) {
            close_descriptor(ends[1]);
            (void)std::fclose(read_); // NOLINT(cppcoreguidelines-owning-memory): the stream is owned
            throw std::runtime_error("cannot open a stream on a pipe");
        }
    }

    ~pipe_streams()
    {
        close_write_end();
        (void)std::fclose(read_); // NOLINT(cppcoreguidelines-owning-memory): the stream is owned
    }

    pipe_streams(const pipe_streams&) = delete;
    pipe_streams& operator=(const pipe_streams&) = delete;

    [[nodiscard]] std::FILE* read_end() const noexcept
    {
        return read_;
    }

    [[nodiscard]] std::FILE* write_end() const noexcept
    {
        return write_;
    }

    // Closes the end that writes, so that the other end reads to the end of the data.
    void close_write_end() noexcept
    {
        if (write_ != nullptr) {
            (void)std::fclose(write_); // NOLINT(cppcoreguidelines-owning-memory): the stream is owned
            write_ = nullptr;
        }
    }

private:
    // A stream on the descriptor, which then owns it; null when there is none, and the descriptor is still open.
    static std::FILE* open(int descriptor, const char* mode) noexcept
    {
#if defined(_WIN32)
        return _fdopen(descriptor, mode);
#else
        return ::fdopen(descriptor, mode); // NOLINT(cppcoreguidelines-owning-memory): pipe_streams closes it
#endif
    }

    static void close_descriptor(int descriptor) noexcept
    {
#if defined(_WIN32)
        (void)_close(descriptor);
#else
        (void)::close(descriptor);
#endif
    }

    std::FILE* read_ = nullptr;
    std::FILE* write_ = nullptr;
};

// A buffer that can only append, like a pipe.
class append_only_buffer : public std::streambuf
{
public:
    [[nodiscard]] const std::string& content() const noexcept
    {
        return content_;
    }

protected:
    int_type overflow(int_type c) override
    {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            content_ += traits_type::to_char_type(c);
        }
        return traits_type::not_eof(c);
    }

private:
    std::string content_;
};

// A buffer over a string of a fixed size that seeks with seekoff() alone, as many buffers written by hand do: the
// seekpos() of std::streambuf fails.
class seekoff_only_buffer : public std::streambuf
{
public:
    explicit seekoff_only_buffer(std::string content) : content_(std::move(content))
    {
        char* begin = content_.data();
        setg(begin, begin, begin + content_.size());
        setp(begin, begin + content_.size());
    }

    [[nodiscard]] const std::string& content() const noexcept
    {
        return content_;
    }

protected:
    pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode which) override
    {
        const auto size = static_cast<off_type>(content_.size());
        off_type base = 0;
        if (direction == std::ios::cur) {
            base = (which & std::ios::out) != 0 ? pptr() - pbase() : gptr() - eback();
        } else if (direction == std::ios::end) {
            base = size;
        }
        const off_type target = base + offset;
        if (target < 0 || target > size) {
            return {off_type{-1}};
        }
        if ((which & std::ios::in) != 0) {
            setg(eback(), eback() + target, egptr());
        }
        if ((which & std::ios::out) != 0) {
            setp(pbase(), epptr());
            pbump(static_cast<int>(target));
        }
        return {target};
    }

private:
    std::string content_;
};

TEST(stdio_source, reads_from_its_start_position_to_the_end)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("header0123456789"));
    const c_file file(directory.path() / "data.bin", "rb");
    ASSERT_EQ(std::fseek(file.get(), 6, SEEK_SET), 0);
    const openxisf::stdio_source source(file.get());
    std::array<std::byte, 4> destination{};

    EXPECT_EQ(source.size(), 10U);
    EXPECT_FALSE(source.supports_concurrent_reads());
    source.read(6, destination);
    EXPECT_EQ(text(destination), "6789");
    source.read(0, destination);
    EXPECT_EQ(text(destination), "0123");
    source.read(10, {});
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(7, destination); }));
}

// The C library may hold a block of the file in its buffer, which a read at an offset outside that block bypasses. BSD
// libraries read the block that holds the target of a seek, so the constructor's seek to the end fills the buffer.
TEST(stdio_source, a_stream_that_shrinks_after_construction_is_end_of_data)
{
    const temp_directory directory;
    const std::vector<std::byte> data = openxisf::test::pattern(std::size_t{1} << 20);
    write_file(directory.path() / "data.bin", data);
    const c_file file(directory.path() / "data.bin", "rb");
    const openxisf::stdio_source source(file.get());
    std::filesystem::resize_file(directory.path() / "data.bin", 1000);
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(500'000, destination); }));
    // What remains is still read.
    source.read(996, destination);
    EXPECT_EQ(text(destination), text(std::span(data).subspan(996, 4)));
}

TEST(stdio_source, refuses_a_stream_that_cannot_seek)
{
    const pipe_streams pipe;
    EXPECT_TRUE(throws<io_error>(errc::not_seekable, [&] { openxisf::stdio_source source(pipe.read_end()); }));
}

TEST(stdio_source, refuses_a_null_stream)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { openxisf::stdio_source source(nullptr); }));
}

TEST(stdio_sink, writes_and_rewrites_from_its_start_position)
{
    const temp_directory directory;
    {
        const c_file file(directory.path() / "unit.xisf", "w+b");
        ASSERT_GE(std::fputs("prefix", file.get()), 0);
        openxisf::stdio_sink sink(file.get());

        EXPECT_TRUE(sink.can_rewrite());
        sink.write(bytes("0123456789"));
        sink.rewrite(2, bytes("ab"));
        sink.write(bytes("!"));
        EXPECT_EQ(sink.position(), 11U);
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(10, bytes("xy")); }));
        sink.finish();
    }

    EXPECT_EQ(read_file(directory.path() / "unit.xisf"), "prefix01ab456789!");
}

TEST(stdio_sink, a_stream_that_cannot_be_written_is_write_failed)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("read only"));
    const c_file file(directory.path() / "data.bin", "rb");
    openxisf::stdio_sink sink(file.get());

    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { sink.write(bytes("abc")); }));
    EXPECT_EQ(sink.position(), 0U);
}

TEST(stdio_sink, a_rewrite_or_a_finish_that_cannot_write_is_write_failed)
{
    // The bytes that the stream buffered are written when a rewrite seeks or when the sink finishes, by then to a
    // descriptor that only reads, like a medium that fails after the first bytes.
    const temp_directory directory;
    write_file(directory.path() / "read_only.bin", bytes("x"));
    for (const bool rewrite : {true, false}) {
        const c_file file(directory.path() / "unit.xisf", "w+b");
        openxisf::stdio_sink sink(file.get());
        sink.write(bytes("0123456789"));
        const c_file read_only(directory.path() / "read_only.bin", "rb");
        redirect(file.get(), read_only.get());
        if (rewrite) {
            EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { sink.rewrite(2, bytes("ab")); }));
        } else {
            EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { sink.finish(); }));
        }
    }
}

TEST(stdio_sink, writes_a_stream_that_cannot_seek_in_order)
{
    pipe_streams pipe;
    openxisf::stdio_sink sink(pipe.write_end());
    EXPECT_FALSE(sink.can_rewrite());
    sink.write(bytes("abc"));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(0, bytes("x")); }));
    sink.finish();
    EXPECT_EQ(sink.position(), 3U);

    pipe.close_write_end();
    std::array<char, 4> received{};
    EXPECT_EQ(std::fread(received.data(), 1, received.size(), pipe.read_end()), 3U);
    EXPECT_EQ(std::string_view(received.data(), 3), "abc");
}

TEST(stdio_sink, refuses_a_null_stream)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { openxisf::stdio_sink sink(nullptr); }));
}

TEST(istream_source, reads_from_its_start_position_to_the_end)
{
    std::istringstream stream("header0123456789");
    stream.seekg(6);
    const openxisf::istream_source source(stream);
    std::array<std::byte, 4> destination{};

    EXPECT_EQ(source.size(), 10U);
    EXPECT_FALSE(source.supports_concurrent_reads());
    source.read(6, destination);
    EXPECT_EQ(text(destination), "6789");
    source.read(0, destination);
    EXPECT_EQ(text(destination), "0123");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(7, destination); }));
}

TEST(istream_source, reads_a_buffer_that_seeks_by_offset_alone)
{
    // The source seeks the way it measured the stream, from its start, with seekoff().
    seekoff_only_buffer buffer("header0123456789");
    std::istream stream(&buffer);
    stream.seekg(6, std::ios::beg);
    const openxisf::istream_source source(stream);
    std::array<std::byte, 4> destination{};

    EXPECT_EQ(source.size(), 10U);
    source.read(6, destination);
    EXPECT_EQ(text(destination), "6789");
    source.read(0, destination);
    EXPECT_EQ(text(destination), "0123");
}

TEST(istream_source, refuses_a_stream_that_cannot_seek)
{
    append_only_buffer buffer;
    std::istream stream(&buffer);

    EXPECT_TRUE(throws<io_error>(errc::not_seekable, [&] { openxisf::istream_source source(stream); }));
}

TEST(istream_source, a_stream_that_ends_early_is_end_of_data_and_one_that_fails_is_read_failed)
{
    short_buffer short_content("0123456789");
    std::istream ending(&short_content);
    const openxisf::istream_source ending_source(ending);
    failing_buffer failing_content("0123456789");
    std::istream failing(&failing_content);
    const openxisf::istream_source failing_source(failing);
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { ending_source.read(0, destination); }));
    EXPECT_TRUE(throws<io_error>(errc::read_failed, [&] { failing_source.read(0, destination); }));
}

TEST(istream_source, the_exceptions_of_a_stream_that_asks_for_them_pass_through)
{
    // stream_io.h: with badbit in the exceptions() mask, the stream rethrows what its buffer throws.
    failing_buffer content("0123456789");
    std::istream stream(&content);
    stream.exceptions(std::ios::badbit);
    const openxisf::istream_source source(stream);
    std::array<std::byte, 4> destination{};

    EXPECT_THROW(source.read(0, destination), injected_fault);
}

TEST(istream_source, reads_again_after_a_failed_read)
{
    // The failure sets badbit, which the source clears before each read.
    failing_once_buffer content("0123456789");
    std::istream stream(&content);
    const openxisf::istream_source source(stream);
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::read_failed, [&] { source.read(2, destination); }));
    source.read(2, destination);
    EXPECT_EQ(text(destination), "2345");
}

TEST(ostream_sink, writes_and_rewrites_from_its_start_position)
{
    std::ostringstream stream;
    stream << "prefix";
    openxisf::ostream_sink sink(stream);

    EXPECT_TRUE(sink.can_rewrite());
    sink.write(bytes("0123456789"));
    sink.rewrite(2, bytes("ab"));
    sink.write(bytes("!"));
    EXPECT_EQ(sink.position(), 11U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(10, bytes("xy")); }));
    sink.finish();

    EXPECT_EQ(stream.str(), "prefix01ab456789!");
}

TEST(ostream_sink, rewrites_a_buffer_that_seeks_by_offset_alone)
{
    seekoff_only_buffer buffer("prefix................");
    std::ostream stream(&buffer);
    stream.seekp(6, std::ios::beg);
    openxisf::ostream_sink sink(stream);

    EXPECT_TRUE(sink.can_rewrite());
    sink.write(bytes("0123456789"));
    sink.rewrite(2, bytes("ab"));
    sink.write(bytes("!"));
    sink.finish();

    EXPECT_EQ(buffer.content(), "prefix01ab456789!.....");
}

TEST(ostream_sink, cannot_rewrite_a_stream_that_cannot_seek)
{
    append_only_buffer buffer;
    std::ostream stream(&buffer);
    openxisf::ostream_sink sink(stream);

    sink.write(bytes("abc"));
    sink.finish();

    EXPECT_FALSE(sink.can_rewrite());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(0, bytes("x")); }));
    EXPECT_EQ(sink.position(), 3U);
    EXPECT_EQ(buffer.content(), "abc");
}

TEST(ostream_sink, a_stream_that_fails_is_write_failed)
{
    failing_buffer buffer;
    std::ostream stream(&buffer);
    openxisf::ostream_sink writing(stream);

    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { writing.write(bytes("abc")); }));
    EXPECT_EQ(writing.position(), 0U);

    failing_buffer flushed;
    std::ostream flushing(&flushed);
    openxisf::ostream_sink finishing(flushing);
    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { finishing.finish(); }));

    failing_later_buffer rewritten;
    std::ostream rewriting(&rewritten);
    openxisf::ostream_sink rewriting_sink(rewriting);
    rewriting_sink.write(bytes("0123456789"));
    rewritten.fail();
    EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { rewriting_sink.rewrite(2, bytes("ab")); }));
}

TEST(ostream_sink, the_exceptions_of_a_stream_that_asks_for_them_pass_through)
{
    // stream_io.h: with badbit in the exceptions() mask, the stream rethrows what its buffer throws.
    failing_buffer buffer;
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit);
    openxisf::ostream_sink sink(stream);

    EXPECT_THROW(sink.write(bytes("abc")), injected_fault);
}

} // namespace
