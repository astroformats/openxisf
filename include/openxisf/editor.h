// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/export.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// @file
/// Writing the pixel data of a unit in place.

namespace openxisf {

/// Options for writing the pixel data of an image in place (editor::write_pixels()). Each member has the meaning of its
/// counterpart in pixel_read_options, for the data given instead of a destination.
struct pixel_write_options
{
    /// The storage model of the samples given: that of the image when empty (spec §8.5.3). Samples in the other model
    /// are converted, through a buffer of the size of the region.
    std::optional<openxisf::pixel_storage> storage{};
    /// Called as the data are written, and able to cancel the write. Its units of work are the bytes of the region.
    progress_function progress{};
    /// The region of the image to write: the samples of its box and channels alone, in rows (pitch). The whole image
    /// when empty.
    std::optional<pixel_region> region{};
    /// The samples from the start of one row of the data given to the start of the next, at least a row, so that a
    /// tile written from a larger buffer needs no copy. 0 for rows that follow each other. The rows are those of
    /// pixel_read_options::pitch.
    std::uint64_t pitch = 0;
    /// Verify a block that has a checksum before its first write (editor). false is the explicit choice of the
    /// application to write the block without verifying it, which then answers for the bytes of the block that
    /// commit() covers with a digest: a block whose digest it knows to be stale, such as that of an output that an
    /// editor left without commit(), after a failure, and that it fills again. The block is not verified afterwards.
    bool verify_checksum = true;
};

#if defined(_MSC_VER)
// The editor holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// A monolithic file (spec §9.2) opened to write the pixel data of its images in place: an image whose pixels are
/// computed region by region, such as the output of a stacking, which writer::add_reserved_image() wrote as zeros, is
/// filled one region at a time, each written once, at its place in the file, without holding the image.
///
/// The editor opens the unit as a reader opens it, with the options of a reader, and describes its images: images(),
/// image() and diagnostics() are those of a reader, and so is read_pixels(). write_pixels() writes the samples of a
/// region straight into the data block of an image, run by run, in the byte order of the block and the storage model of
/// the image, and leaves every other byte of the file as it is: nothing is read back to fill the gaps between the runs.
/// Runs that follow each other in the block are one write, such as a band of full rows of each channel of a planar
/// image; in normal storage, the samples of a subset of the channels are one write for each pixel, so an image that is
/// filled by regions is better planar, as an output frame is.
///
/// A block can be written in place when it is attached, not compressed, and shares no byte with another block, and,
/// when it has a checksum, when its checksum attribute is written as algorithm:digest on the element of the image, so
/// that a digest of the same size replaces it, as the writer writes it: every image that the writer writes
/// uncompressed, a reserved one among them, with or without checksums. Any other block throws unsupported_error with
/// errc::not_editable, as a header file does when it is opened: a distributed unit is not editable.
///
/// A block with a checksum is verified once, before its first write: the editor reads it whole, in pieces, and refuses
/// to write a block that fails its checksum (errc::checksum_mismatch), so that commit() gives a digest only to bytes
/// that were verified or written, unless the application writes it with pixel_write_options::verify_checksum false.
/// Concurrent first writes of a block wait for that one verification.
///
/// The checksum of a block written is stale until commit(), which computes the digest of each block written since the
/// open or the last commit(), writes it in place in the header, and flushes the device. Meanwhile a reader of the file
/// finds such a block corrupt (errc::checksum_mismatch), and the editor reads it without verifying it; a read of the
/// editor that verifies a block whose first write begins while it is read is made again so, its progress counted from
/// 0 again. An editor destroyed without commit() leaves those digests stale, so that an output left incomplete by a
/// failure is not taken for a complete one, and an editor that fills it again writes it without verifying it
/// (pixel_write_options::verify_checksum); a block without a checksum gives no such sign.
///
/// The editor owns its device, so a file stays open as long as its editor exists. read_pixels() and write_pixels() can
/// be called from any number of threads at once, for regions that share no sample: the regions of different images, or
/// of one image apart, such as the tiles of a frame filled by several threads. So can images(), image() and
/// diagnostics(). A read of a region reads the bytes of its region alone, never those between its rows, so that such
/// reads and writes never touch the same bytes of the device. commit() needs exclusive access. A reader of the file
/// meanwhile reads each region as it is then. An editor can be moved, and a moved-from editor can only be destroyed or
/// assigned to.
class OPENXISF_API editor
{
public:
    /// Opens the file at path, which is UTF-8 on every platform, for reading and writing (file_device), with the
    /// options of a reader. The resolver of options is not used: a monolithic file has no external data blocks.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the file cannot be opened for reading and writing, or read.
    /// @throws unsupported_error (errc::not_editable) when the file is a header file (spec §9.3).
    /// @throws invalid_data_error, unsupported_error, limit_error or integrity_error as a reader opening the file
    ///         throws them.
    explicit editor(std::string_view path, read_options options = {});

    /// Opens the unit in device, with the same exceptions as the other constructor. Exceptions of the device pass
    /// through unchanged.
    /// @throws usage_error when device is null.
    explicit editor(std::unique_ptr<random_access_device> device, read_options options = {});

    editor(const editor&) = delete;
    editor& operator=(const editor&) = delete;
    editor(editor&& other) noexcept;
    editor& operator=(editor&& other) noexcept;
    ~editor();

    /// The images of the unit, as reader::images() lists them.
    [[nodiscard]] std::span<const image_info> images() const noexcept;

    /// The image at index in images().
    /// @throws usage_error when index is out of range.
    [[nodiscard]] const image_info& image(std::size_t index) const;

    /// The problems found while opening the unit, as reader::diagnostics() lists them.
    [[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;

    /// Reads the pixel data of the image at index, as reader::read_pixels() reads them, but a block written since the
    /// open or the last commit(), whose checksum is stale, is read without verifying it.
    [[nodiscard]] std::vector<std::byte> read_pixels(std::size_t index, const pixel_read_options& options = {}) const;

    /// Reads the pixel data of the image at index into destination, as reader::read_pixels() reads them, but a block
    /// written since the open or the last commit() is read without verifying it.
    void read_pixels(std::size_t index, std::span<std::byte> destination, const pixel_read_options& options = {}) const;

    /// Reads the samples of the image at index as values of T, the type of its sample format, as reader::read_pixels()
    /// reads them.
    template <pixel_sample T>
    [[nodiscard]] std::vector<T> read_pixels(std::size_t index, const pixel_read_options& options = {}) const
    {
        std::vector<T> samples(sample_count_of(index, sample_format_of<T>(), options));
        read_pixels(index, std::as_writable_bytes(std::span(samples)), options);
        return samples;
    }

    /// Reads the samples of the image at index into destination, as values of T, the type of its sample format, as
    /// reader::read_pixels() reads them.
    template <pixel_sample T>
    void read_pixels(std::size_t index, std::span<T> destination, const pixel_read_options& options = {}) const
    {
        check_sample_format(index, sample_format_of<T>());
        read_pixels(index, std::as_writable_bytes(destination), options);
    }

    /// Writes data, samples in native byte order in the storage model of options.storage, or in that of the image, into
    /// the data block of the image at index, in place: the samples of options.region, in its rows, or of the whole
    /// image. data has exactly the bytes of the region, or with options.pitch at least the bytes from the start of its
    /// first row to the end of the last, of which the rows alone are written. The bytes reach the device as they are
    /// written. When it throws, part of the region may be written, and the checksum of the block is stale all the same.
    /// @throws usage_error when index is out of range, the region or the pitch of options do not fit the image, or data
    ///         has another size; nothing is written then.
    /// @throws unsupported_error (errc::not_editable) when the block of the image cannot be written in place, and
    ///         integrity_error (errc::checksum_mismatch) when it fails its checksum before its first write; nothing is
    ///         written then.
    /// @throws limit_error (errc::allocation_too_large) when data in the other storage model need a buffer of the size
    ///         of the region beyond limits::max_allocation.
    /// @throws cancelled_error when options.progress returns false.
    /// @throws io_error, or what a custom device throws, when the device fails.
    void write_pixels(std::size_t index, std::span<const std::byte> data, const pixel_write_options& options = {});

    /// Writes samples, values of T, the type of the sample format of the image at index, as the other write_pixels()
    /// writes data.
    /// @throws usage_error when T is another type; and what the other write_pixels() throws.
    template <pixel_sample T>
    void write_pixels(std::size_t index, std::span<const T> samples, const pixel_write_options& options = {})
    {
        check_sample_format(index, sample_format_of<T>());
        write_pixels(index, std::as_bytes(samples), options);
    }

    /// Writes samples that are not const, such as std::span(samples) of a std::vector<float> that is not const, as the
    /// overload for const samples does.
    template <pixel_sample T>
    void write_pixels(std::size_t index, std::span<T> samples, const pixel_write_options& options = {})
    {
        write_pixels(index, std::span<const T>(samples), options);
    }

    /// Computes the digest of each block written since the open or the last commit() that has a checksum, from its
    /// bytes in the file, read in pieces, and writes it in place of the old one in the header, then flushes the device,
    /// so that the file is complete and consistent. The editor then verifies those blocks again when it reads them.
    /// When it throws, the blocks whose digests were not written stay to be committed.
    /// @throws io_error, or what a custom device throws, when the device fails.
    void commit();

private:
    // The number of samples that a typed read of the image at index with options allocates, after the checks of
    // reader::read_pixels().
    [[nodiscard]] std::size_t sample_count_of(std::size_t index, sample_format format,
                                              const pixel_read_options& options) const;

    // Checks that the image at index has the given sample format.
    void check_sample_format(std::size_t index, sample_format format) const;

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openxisf
