// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Sources and sinks: where units are read from and written to, the progress of reading and writing, and the resolvers
/// that open the external files of distributed units.

namespace openxisf {

/// Reports the progress of a long operation, such as reading pixel data: done of total units of work, in bytes, from
/// 0 to total. Returning false cancels the operation, which then throws cancelled_error. It is called on the thread
/// that runs the operation, and its exceptions pass through unchanged.
using progress_function = std::function<bool(std::uint64_t done, std::uint64_t total)>;

#if defined(_MSC_VER)
// The built-in sources and sinks hold standard-library members, which cl reports for an exported class. A C++
// interface requires the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// Random-access input that units are read from: a file, a buffer, or anything an application implements.
///
/// Reads are positional and exact, so an implementation needs no cursor. The library never asks for bytes beyond
/// size(), and calls read() from one thread at a time unless supports_concurrent_reads() is true. The content must not
/// change while a unit is read from the source. Sources are not copyable; the library holds them by pointer.
class OPENXISF_API input_source
{
public:
    input_source() = default;
    input_source(const input_source&) = delete;
    input_source& operator=(const input_source&) = delete;
    virtual ~input_source();

    /// Size of the content, in bytes.
    [[nodiscard]] virtual std::uint64_t size() const = 0;

    /// Fills destination with the bytes that start at offset, or throws io_error. The built-in sources throw it with
    /// errc::end_of_data when the bytes extend beyond size().
    virtual void read(std::uint64_t offset, std::span<std::byte> destination) const = 0;

    /// True when read() may run on several threads at once. False by default.
    [[nodiscard]] virtual bool supports_concurrent_reads() const;

    /// Names the source in error messages, for example with a path. Empty by default.
    [[nodiscard]] virtual std::string description() const;
};

/// Output that units are written to: a file, a buffer, or anything an application implements.
///
/// Bytes are appended with write(). A sink that can also rewrite bytes it already holds lets the writer stream a unit
/// with bounded memory. finish() follows the last byte of a complete unit; a sink destroyed without it holds an
/// incomplete one, which file_sink discards. A sink receives one unit, or one file of a distributed unit, from its
/// start: the writer refuses a sink that already holds bytes. Sinks are not copyable.
class OPENXISF_API output_sink
{
public:
    output_sink() = default;
    output_sink(const output_sink&) = delete;
    output_sink& operator=(const output_sink&) = delete;
    virtual ~output_sink();

    /// Appends data, or throws io_error.
    virtual void write(std::span<const std::byte> data) = 0;

    /// Number of bytes written so far, which is the offset of the next one.
    [[nodiscard]] virtual std::uint64_t position() const = 0;

    /// True when rewrite() is supported. False by default.
    [[nodiscard]] virtual bool can_rewrite() const;

    /// Overwrites bytes already written, without changing position(). offset + data.size() must not exceed position().
    /// The default implementation throws usage_error.
    virtual void rewrite(std::uint64_t offset, std::span<const std::byte> data);

    /// Completes the output after its last byte: flushes it, and commits it where that applies. Nothing may be written
    /// afterwards, which file_sink refuses and other sinks need not check. Does nothing by default.
    virtual void finish();
};

/// A regular file, read with positional I/O, so that any number of threads can read it at once.
///
/// The file stays open until the source is destroyed. Other programs may still read, write, rename or delete it, but
/// its content must not change while a unit is read from it.
class OPENXISF_API file_source final : public input_source
{
public:
    /// Opens the file at path, which is UTF-8 on every platform. What the system says is not a regular file is refused
    /// before it is opened, since opening a device can act on it: on Windows, a device name such as NUL in any
    /// directory.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the file cannot be opened (errc::open_failed) or is not a regular file, such as a
    ///         directory, the root of a drive or a share among them, or a device (errc::not_a_regular_file).
    explicit file_source(std::string_view path);
    ~file_source() override;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;
    /// The path.
    [[nodiscard]] std::string description() const override;

private:
    struct state;
    std::unique_ptr<const state> state_;
};

/// Options of file_sink.
struct file_sink_options
{
    /// Flush the file to the storage device before it replaces the target, so that it survives a power failure.
    /// Slower.
    bool flush_to_disk = false;
};

/// A file that is replaced atomically. The output goes to a temporary file in the target's directory, which replaces
/// the target when finish() succeeds. Until then the target keeps its old content, or does not exist. A sink destroyed
/// before finish(), after a failure or a cancellation, removes its temporary file, and so does a finish() that fails.
///
/// The temporary file is named `.openxisf-<16 hexadecimal digits>.tmp`. Only a process that ends abruptly leaves one
/// behind. Supports rewrite().
///
/// The new file is created like any new file: it has the permissions that the system gives one, not those of the file
/// it replaces. A target that exists must be a regular file or a link: a directory, a FIFO, a socket or a device is
/// refused before anything is created. A target that is a symbolic link, or a junction on Windows, is itself replaced
/// by the new file; what it points to is left as it is. On Windows, a target that another program has open is replaced
/// only when that program let others delete it (`FILE_SHARE_DELETE`), as file_source does, and a target with the
/// read-only attribute is not replaced; otherwise finish() fails. That needs a file system that renames with POSIX
/// semantics, as NTFS does from Windows 10 1607 on: on others, such as FAT32, exFAT and some network shares, finish()
/// fails while any program has the target open, a reader of the unit being replaced included. POSIX systems replace a
/// target that cannot be written, as the directory allows.
class OPENXISF_API file_sink final : public output_sink
{
public:
    /// Creates the temporary file for a target at path, which is UTF-8 on every platform. A relative path is taken
    /// from the current directory now: finish() replaces that target, whatever the current directory is by then.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the target is a directory, a FIFO, a socket or a device, such as /dev/null, or NUL on
    ///         Windows in any directory (errc::not_a_regular_file); when the temporary file cannot be created, or the
    ///         current directory of a relative path cannot be found (errc::open_failed); or when the system provides no
    ///         random data for its name (errc::entropy_unavailable).
    explicit file_sink(std::string_view path, file_sink_options options = {});
    ~file_sink() override;

    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    /// True.
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;
    /// Replaces the target with the temporary file. When it throws, the temporary file is gone, and the target keeps
    /// its old content unless the failure was the flush to disk that follows the replacement.
    void finish() override;

private:
    struct state;
    std::unique_ptr<state> state_;
};

/// Bytes in memory, borrowed from the caller or owned by the source. Any number of threads can read it at once.
class OPENXISF_API memory_source final : public input_source
{
public:
    /// Borrows data, which must outlive the source.
    explicit memory_source(std::span<const std::byte> data) noexcept;
    /// Owns data. A vector that is not moved in is copied, so pass a std::span to borrow it instead.
    explicit memory_source(std::vector<std::byte> data) noexcept;

    [[nodiscard]] std::uint64_t size() const override;
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    /// True.
    [[nodiscard]] bool supports_concurrent_reads() const override;

private:
    std::vector<std::byte> owned_;
    std::span<const std::byte> data_;
};

/// Bytes in memory, in a buffer that grows as needed. Supports rewrite().
class OPENXISF_API memory_sink final : public output_sink
{
public:
    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    /// True.
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;

    /// The bytes written so far.
    [[nodiscard]] std::span<const std::byte> data() const noexcept;
    /// Moves the bytes written so far out of the sink, which is empty afterwards.
    [[nodiscard]] std::vector<std::byte> release() noexcept;

private:
    std::vector<std::byte> data_;
};

/// Options of callback_source.
struct callback_source_options
{
    /// The read function may run on several threads at once.
    bool concurrent_reads = false;
    /// Names the source in error messages.
    std::string description{};
};

/// A source that calls a function of the application to read. The function is never asked for bytes beyond the size.
class OPENXISF_API callback_source final : public input_source
{
public:
    /// Fills destination with the bytes that start at offset, as input_source::read() does.
    using read_function = std::function<void(std::uint64_t offset, std::span<std::byte> destination)>;

    /// A source of size bytes that reads with read.
    /// @throws usage_error when read is empty.
    callback_source(std::uint64_t size, read_function read, callback_source_options options = {});

    [[nodiscard]] std::uint64_t size() const override;
    /// Calls the read function, whose exceptions pass through unchanged.
    void read(std::uint64_t offset, std::span<std::byte> destination) const override;
    [[nodiscard]] bool supports_concurrent_reads() const override;
    [[nodiscard]] std::string description() const override;

private:
    std::uint64_t size_;
    read_function read_;
    callback_source_options options_;
};

/// A sink that calls functions of the application to write. Their exceptions pass through unchanged, and position()
/// counts the bytes of the write calls that returned.
class OPENXISF_API callback_sink final : public output_sink
{
public:
    /// Appends data, as output_sink::write() does.
    using write_function = std::function<void(std::span<const std::byte> data)>;
    /// Overwrites bytes already written, as output_sink::rewrite() does. Never asked for bytes beyond position().
    using rewrite_function = std::function<void(std::uint64_t offset, std::span<const std::byte> data)>;
    /// Completes the output, as output_sink::finish() does.
    using finish_function = std::function<void()>;

    /// A sink that appends with write. It supports rewrite() when a rewrite function is given, and finish() calls the
    /// finish function when one is given.
    /// @throws usage_error when write is empty.
    explicit callback_sink(write_function write, rewrite_function rewrite = {}, finish_function finish = {});

    void write(std::span<const std::byte> data) override;
    [[nodiscard]] std::uint64_t position() const override;
    [[nodiscard]] bool can_rewrite() const override;
    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override;
    void finish() override;

private:
    write_function write_;
    rewrite_function rewrite_;
    finish_function finish_;
    std::uint64_t position_ = 0;
};

/// The forms of the location of an external data block (spec §10.3).
enum class location_form : std::uint8_t
{
    url,           ///< `url(URL)`: a local or remote resource.
    absolute_path, ///< `path(/file/path)`: a local file, by its absolute path in UNIX syntax.
    relative_path, ///< `path(@header_dir/file/path)`: a local file, by its path from the directory of the header file.
};

/// A file that holds external data blocks of a distributed unit (spec §10.2): a data blocks file (`.xisb`), or any
/// other file, whose whole content is one data block.
struct external_reference
{
    location_form form = location_form::relative_path;
    /// The URL, the absolute path, or the path from the directory of the header file without its `@header_dir/`
    /// prefix, as the location attribute writes it: `blocks/unit.xisb` for `path(@header_dir/blocks/unit.xisb)`.
    std::string location{};
};

/// Opens the file of an external reference, for a reader. Returning null refuses it. Throwing io_error or
/// unsupported_error says that it cannot be opened, or must not be. The data blocks in the file are then unavailable,
/// with an error diagnostic of the code of the exception, or errc::unsupported_location for null; a strict open, and a
/// read of such a block, throw an exception of the same class and code, with the system error of an io_error. So does
/// an io_error of the source that the resolver returned: while the block index of a data blocks file is read, for the
/// blocks in the file, and while ancillary data are loaded from the file (the value of a property, an ICC profile, the
/// pixels of a thumbnail), at the open or in reader::load_ancillary_data(), for the block being loaded. Any other
/// exception passes through and fails the open. A resolver is called while a unit is opened, on that thread, once for
/// each file, and again when reader::load_ancillary_data() opens the unit again; the reader keeps the source it returns
/// as long as it exists.
using external_resolver = std::function<std::unique_ptr<input_source>(const external_reference& reference)>;

/// Options of file_resolver().
struct file_resolver_options
{
    /// Open absolute paths. They name any file of the system, so they are refused by default
    /// (errc::location_not_allowed). On Windows, `/c/dir/file` names `C:\dir\file`, a network path (`//server/share`)
    /// is refused, and any other absolute path names no file (errc::unsupported_location).
    bool absolute_paths = false;
};

/// A resolver of `path()` locations to regular files. A relative path is resolved from header_directory, and must lead
/// to a file inside that directory once symbolic links are followed (errc::location_not_allowed otherwise). Inside is a
/// matter of paths: a hard link in the directory to a file elsewhere, or a directory on which another file system is
/// mounted, is inside. Paths follow UNIX syntax (spec §10.3), so on Windows a path with a backslash, a colon or another
/// character that a file name cannot hold names no file (errc::unsupported_location). `url()` locations are refused
/// (errc::unsupported_location).
///
/// A missing file is errc::open_failed, and a file that is not a regular file, such as a directory or a device,
/// errc::not_a_regular_file, as for file_source, which refuses a device before it is opened. A relative path is
/// resolved as POSIX resolves one on every system: a `.` or `..` step follows a directory (errc::open_failed after a
/// file, as in `data.xisb/.`), and a path that ends with a separator or such a step names a directory. The checks
/// protect against what a unit names, not against programs that change the directory tree while a unit is opened. A
/// relative header_directory is taken from the current directory when the resolver is made, so that a unit opened by a
/// relative path finds its files when it is read again from another one.
/// @throws usage_error when header_directory is empty or not valid UTF-8.
/// @throws io_error (errc::open_failed) when header_directory is relative and the current directory cannot be found.
[[nodiscard]] OPENXISF_API external_resolver file_resolver(std::string_view header_directory,
                                                           file_resolver_options options = {});

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openxisf
