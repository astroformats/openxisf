// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/export.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/limits.h>
#include <openxisf/property.h>
#include <openxisf/types.h>
#include <openxisf/writer.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// @file
/// Reading XISF units.

namespace openxisf {

/// How a unit is stored (spec §9.1).
enum class unit_storage
{
    monolithic,  ///< A monolithic file (`.xisf`): the header and the attached data blocks in one file.
    distributed, ///< A header file (`.xish`), whose data blocks are external.
};

/// Whether a unit has an XML signature (spec §9.5).
enum class signature_status
{
    none,         ///< The unit is not signed.
    not_verified, ///< The unit is signed. OpenXISF does not verify signatures.
};

/// Options for opening a unit.
struct read_options
{
    /// Fail with the error of the first diagnostic of severity::error, instead of opening the unit with that object
    /// unavailable. The exception tells the kind of error: integrity_error for data that fail their checksum,
    /// unsupported_error for a feature that OpenXISF does not support, such as an unknown compression codec, or a
    /// location that the resolver does not allow, limit_error for data beyond a limit, io_error for an external file
    /// that cannot be opened or read, and invalid_data_error for anything else.
    bool strict = false;
    /// Read the header alone: leave the ancillary data in data blocks unloaded, so that nothing but the header is read.
    /// ICC profiles and the pixels of thumbnails stay empty, and the properties and tables with values in data blocks
    /// are left out, until reader::load_ancillary_data() loads them; the problems of those data are found then. Their
    /// identifiers are known: another property or table of the object with one of them is refused, as a full open
    /// refuses it, and a mandatory metadata property left out is not reported missing. The model is not complete until
    /// then, so the writer would lose those data, or refuse a thumbnail without pixels (writer).
    ///
    /// The external files of a distributed unit stay closed too: the resolver opens a file, and its block index is
    /// read, when the pixels of an image in it are first read, or by reader::load_ancillary_data(), so that a resolver
    /// that is costly to call, such as one of a network store, is called only for the files that are used. The
    /// problems of a file are then found by the read, not by the open, whose diagnostics do not list them and which
    /// does not fail for them when strict: a read of an image in the file throws the error of its block, one that does
    /// not hold the pixel data of the image among them, and load_ancillary_data() lists them.
    bool header_only = false;
    /// Opens the files of the external data blocks of a distributed unit (spec §10.2). When it is empty, a unit opened
    /// from a path gets file_resolver() for the directory of that path, with its default options, and a unit opened
    /// from a source has no external data block available.
    external_resolver resolver{};
    /// Safety limits for untrusted input.
    openxisf::limits limits{};
};

/// Options for reading the pixel data of an image.
struct pixel_read_options
{
    /// The storage model of the samples read: that of the image when empty (spec §8.5.3).
    std::optional<openxisf::pixel_storage> storage{};
    /// Called as the data are read and decompressed, and able to cancel the read. Its units of work are the bytes read
    /// from the source plus, for a compressed image, the bytes decompressed; for a region read as it is stored, without
    /// a decompression or a verification of the whole block, the bytes of the region.
    progress_function progress{};
    /// The region of the image to read: the samples of its box and channels alone, in rows (pitch). The whole image
    /// when empty.
    ///
    /// What a region costs depends on how the image is stored. Uncompressed data are read run by run, the stretches of
    /// the block that the region fills, those close together in one read. A compressed block is decoded subblock by
    /// subblock, those that hold bytes of the region alone, a few at a time, so that a region of a large image needs a
    /// few subblocks of memory, whatever its size: a frame meant for region access is written with subblocks of a few
    /// MiB (write_options::subblock_size, 4 MiB by default), since a block compressed as a single subblock is decoded
    /// whole. Byte shuffling spreads the bytes of the samples over the block, one stretch for each byte of a sample: a
    /// region of a shuffled block decodes about as many bytes as without shuffling, and up to two more subblocks for
    /// each of those stretches, and gathers its samples byte by byte. A block with a checksum is read whole and
    /// verified first, as for a whole image, unless verify_checksum is false.
    std::optional<pixel_region> region{};
    /// The samples from the start of one row of the destination to the start of the next, at least a row, so that the
    /// samples read land in a larger buffer: a tile in its place in a frame, for example. 0 for rows that follow each
    /// other. A row holds the samples along the first axis: of one channel in the planar storage model, where the rows
    /// of each channel follow those of the one before, and of every channel read, pixel by pixel, in the normal model.
    /// The rows follow the order of the pixels along the other axes. The bytes between the rows are left as they are.
    std::uint64_t pitch = 0;
    /// Verify the data of a block that has a checksum before they are used, as spec §10.5 requires. A checksum covers
    /// the whole block, so a region of such a block is read by reading and verifying all of it first. false is the
    /// explicit choice of the application to read the data as those of a block without a checksum, which nothing
    /// verifies then: regions of a unit that it verified once, for example. Inline and embedded blocks are verified
    /// when the unit is opened, either way.
    bool verify_checksum = true;
};

/// Where a data block is stored (spec §10.3).
enum class block_location : std::uint8_t
{
    inline_data, ///< In the header, as the text of the element whose data it holds (`inline:base64`, `inline:hex`).
    embedded,    ///< In the header, as the text of a Data child element of that element (`embedded`).
    attachment,  ///< In the monolithic file, after the header (`attachment:position:size`).
    external,    ///< In a file of its own or in a data blocks file, which the resolver opens (`path()`, `url()`).
};

/// How the pixel data of an image are stored: the facts of its data block that the header states, and for an external
/// block the block index of its file (spec §10), which reader::block() returns. They tell what a read costs before it
/// is made: a region of an uncompressed block is read run by run, and one of a compressed block subblock by subblock,
/// a few at a time, while a block with a checksum is read whole to be verified first (pixel_read_options). They also
/// give the options that write the image again in the same form (write_options).
struct block_info
{
    /// Where the block is stored.
    block_location location = block_location::attachment;
    /// The byte order of the samples in the block (spec §10.4). Reads return them in native byte order, whatever it is.
    std::endian byte_order = std::endian::little;
    /// The codec that compresses the block (spec §10.6); empty when it is not compressed.
    std::optional<openxisf::codec> codec{};
    /// Whether the bytes of the samples were shuffled before they were compressed (spec §10.6.3).
    bool byte_shuffle = false;
    /// The uncompressed size of the largest subblock of the block, in bytes: the most that a read decodes in one piece,
    /// as write_options::subblock_size bounds it. That of the whole block when it is compressed as a single subblock,
    /// and 0 when it is not compressed.
    std::uint64_t subblock_size = 0;
    /// The number of subblocks of the block, which are compressed and decompressed separately (spec §10.6): 1 for a
    /// block compressed as a whole, and 0 when it is not compressed.
    std::uint64_t subblock_count = 0;
    /// The algorithm of the checksum of the block (spec §10.5); empty when it has none.
    std::optional<checksum_algorithm> checksum{};
    /// The size of the block as stored, in bytes: compressed, when it is compressed, and decoded from its text, for an
    /// inline or embedded block.
    std::uint64_t stored_size = 0;
    /// The size of the data of the block, uncompressed, in bytes: that of the pixel data of the image
    /// (image_info::data_size()).
    std::uint64_t uncompressed_size = 0;

    friend bool operator==(const block_info&, const block_info&) = default;
};

#if defined(_MSC_VER)
// The reader holds a standard-library member, which cl reports for an exported class. A C++ interface requires the
// same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// An XISF unit opened for reading.
///
/// The source is recognized by its content: a monolithic file (spec §9.2), or a header file (spec §9.3), which starts
/// with XML. Opening reads and checks the whole header, including where each data block is and how it is encoded, and
/// verifies the checksums of the blocks written in the header; attached blocks are verified when they are read. The
/// external data blocks of a header file are located when it is opened: read_options::resolver opens each file once,
/// and the block index of each data blocks file is read (spec §9.4), so that a block that cannot be found is
/// unavailable, with a diagnostic; the files stay open as long as the reader exists (read_options::header_only leaves
/// them closed until they are used). It describes each image and what is associated with it, and loads the ancillary
/// data in data blocks, within limits::max_ancillary_data: ICC profiles, the pixels of thumbnails, and the values of
/// properties and table cells (read_options::header_only defers them). Pixel data are read on request, with
/// read_pixels(). Problems confined to one object of the unit do not fail it: the object is unavailable, and a
/// diagnostic says why (spec §7). An external file whose source fails with io_error is such a problem, while its block
/// index is read, for the blocks in the file, and while ancillary data are loaded from it, for the block being loaded;
/// an io_error of the source of the unit fails the open. A signed unit is read like any other; its signature is
/// returned, not verified (signature()).
///
/// The reader refuses what affects safety or the meaning of the data, and reads departures from the specification that
/// are unambiguous and harmless with a warning: among others, hexadecimal digits in uppercase, codec and checksum
/// algorithm names in another case than the specification's, FITS keyword names padded with trailing spaces, which are
/// read without them, values of the Image namespace that spec §11.5.3.5 forbids, such as a frame number of 0, which are
/// kept, a property or table that an object contains and names, or names twice, which it has once, and characters that
/// XML 1.0 does not allow, such as control characters other than tab, line feed and carriage return, which are kept as
/// written. A compressed block or subblock whose compressed size equals its uncompressed size is read as stored,
/// without decompression, and one longer than its data is decompressed, as spec §10.6.1 (Revision 2) defines; codec
/// output of exactly the size of its input, which an encoder of an earlier revision could write and which the
/// specification lets a decoder try to decompress first, is read as stored, as PixInsight reads it.
///
/// The reader owns its source, so a file stays open as long as its reader exists. Its const member functions can be
/// called from any number of threads at once, pixel reads included, which open the external files that a header-only
/// open left closed one at a time; load_ancillary_data(), the only function that changes it, needs exclusive access.
/// It can be moved, and a moved-from reader can only be destroyed or assigned to.
class OPENXISF_API reader
{
public:
    /// Opens the file at path, which is UTF-8 on every platform.
    /// @throws usage_error when path is empty or not valid UTF-8.
    /// @throws io_error when the file cannot be opened or read.
    /// @throws invalid_data_error when the file is not an XISF unit, or its header violates the specification in a way
    ///         that leaves the unit without meaning.
    /// @throws unsupported_error when the unit is of an XISF version other than 1.0.
    /// @throws limit_error when the header exceeds one of the limits of options.
    /// @throws integrity_error, unsupported_error, invalid_data_error, limit_error or io_error with strict options, for
    ///         the first error diagnostic (see read_options::strict).
    explicit reader(std::string_view path, read_options options = {});

    /// Opens the unit in source, with the same exceptions as the other constructor. Exceptions of the source pass
    /// through unchanged.
    /// @throws usage_error when source is null.
    explicit reader(std::unique_ptr<input_source> source, read_options options = {});

    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;
    reader(reader&& other) noexcept;
    reader& operator=(reader&& other) noexcept;
    ~reader();

    /// How the unit is stored.
    [[nodiscard]] unit_storage storage() const noexcept;

    /// Whether the unit is signed: whether an XML signature follows its root element (spec §9.5). OpenXISF reads a
    /// signed unit like any other, and does not verify its signature: signature_xml() and signed_xml() give the
    /// application what XML signature tooling of its own needs to verify it. A signature that is malformed, or that
    /// does not name the root element, still makes the unit signed, with a warning (errc::invalid_signature), and
    /// does not prevent reading the rest of it. A Signature element whose own start tag is not well-formed is not
    /// taken for a signature, since the namespace declared there cannot be read: the unit is then not signed, with a
    /// warning (errc::invalid_xml).
    [[nodiscard]] signature_status signature() const noexcept;

    /// The signature of a signed unit: its Signature element, exactly as written in the header, from the '<' of its
    /// start tag to the '>' of its end tag. A signature that is not well-formed XML extends as far as the nesting of
    /// its tags goes. Empty when the unit is not signed.
    [[nodiscard]] std::string_view signature_xml() const noexcept;

    /// What the signature of a signed unit covers: the XISF root element, exactly as written in the header, from the
    /// '<' of its start tag to the '>' of its end tag (spec §9.5). The signature names it by its id attribute, an
    /// identifier for XML signature tooling, and covers the data blocks stored outside the header through their
    /// checksums, which the reader verifies when it reads the blocks; a block of a signed unit without one is a
    /// warning (errc::missing_checksum). Empty when the unit is not signed.
    [[nodiscard]] std::string_view signed_xml() const noexcept;

    /// The properties of the unit: those of its Metadata element, such as XISF:CreationTime (spec §11.4).
    [[nodiscard]] const property_list& metadata() const noexcept;

    /// The standalone properties: the Property elements of the root element (spec §11.1). Standalone properties belong
    /// to no object (spec §8.4), so their identifiers need not be unique: the first of each is listed, among the
    /// properties and tables, with a warning about each other (errc::duplicate_property_id), which a Reference still
    /// associates with an image.
    [[nodiscard]] const property_list& properties() const noexcept;

    /// The standalone table properties: the Table elements of the root element (spec §11.3), in document order, but
    /// for those whose identifier a standalone property or table before them has (properties()).
    [[nodiscard]] std::span<const table> tables() const noexcept;

    /// The images of the unit (spec §11.5), in document order: each Image element, and the image that each Reference
    /// element of the root element names, listed again where the Reference is (spec §11.13). An image listed again
    /// keeps its id, and the writer refuses two images with one id (errc::duplicate_image_id): leave the copy out, or
    /// clear its id, to write the images again. An Image element whose attributes cannot be read is left out, with an
    /// error diagnostic. An image is listed even when its data block is unavailable, for example compressed with a
    /// codec that OpenXISF does not support; read_pixels() then throws the error of that diagnostic.
    [[nodiscard]] std::span<const image_info> images() const noexcept;

    /// The image at index in images().
    /// @throws usage_error when index is out of range.
    [[nodiscard]] const image_info& image(std::size_t index) const;

    /// How the pixel data of the image at index are stored: the facts of its data block (block_info), from the header,
    /// and for an external block from the block index of its file, without a read of the pixel data. An image that a
    /// Reference lists again has the block of the image that it copies. For a unit opened with
    /// read_options::header_only, the external file that holds the block is opened first, as a read of the pixels
    /// opens it.
    /// @throws usage_error when index is out of range.
    /// @throws integrity_error, unsupported_error, invalid_data_error, limit_error or io_error when the data block of
    ///         the image is unavailable, with the code of the error diagnostic about it, as read_pixels() throws them;
    ///         and what the resolver throws besides io_error and unsupported_error when it opens the file of the block
    ///         for a header-only open.
    [[nodiscard]] block_info block(std::size_t index) const;

    /// Reads the pixel data of the image at index: its samples in native byte order, in the storage model of
    /// options.storage, or in that of the image. The data are verified against their checksum and decompressed first.
    /// With options.region, the samples of the region alone, in its rows; with options.pitch, the bytes that the rows
    /// span, with zeros between them.
    /// @throws usage_error when index is out of range, or the region or the pitch of options do not fit the image.
    /// @throws integrity_error when the data fail their checksum or do not decompress to their size.
    /// @throws unsupported_error, invalid_data_error or limit_error when the data block of the image is unavailable, or
    ///         larger than limits::max_allocation, with the code of the error diagnostic about it; and io_error or
    ///         unsupported_error, with that code, when the resolver could not open the external file of the block.
    /// @throws cancelled_error when options.progress returns false.
    /// @throws io_error, or what a custom source throws, when the source fails; and what the resolver throws besides
    ///         io_error and unsupported_error when it opens the file of the block for a header-only open.
    [[nodiscard]] std::vector<std::byte> read_pixels(std::size_t index, const pixel_read_options& options = {}) const;

    /// Reads the pixel data of the image at index into destination, which must have exactly image(index).data_size()
    /// bytes, with the exceptions of the other read_pixels(). Data that keep their storage model are read, or
    /// decompressed, straight into destination, so limits::max_allocation applies only to what the read allocates
    /// besides: the stored bytes of compressed data, and for byte-shuffled data the buffer of their size that they are
    /// unshuffled from. When it throws, destination may hold part of the data, but never data that failed their
    /// checksum.
    ///
    /// With options.region, destination holds the samples of the region: exactly their bytes, or with options.pitch at
    /// least the bytes from the start of the first row to the end of the last, of which it fills the rows alone. A
    /// region allocates besides only what its reads need (pixel_read_options::region): a staging buffer of a few MiB
    /// for runs read together, a few subblocks of a compressed block and their stored bytes, all of the stored bytes of
    /// a block verified whole, and a buffer of the region's size when it changes its storage model.
    /// @throws usage_error when destination has another size.
    void read_pixels(std::size_t index, std::span<std::byte> destination, const pixel_read_options& options = {}) const;

    /// Reads the samples of the image at index as values of T, which must be the type of its sample format
    /// (sample_format_of<T>()): std::uint16_t for a UInt16 image, std::complex<float> for a Complex32 image.
    /// @throws usage_error when T is another type; and what the other read_pixels() throw.
    template <pixel_sample T>
    [[nodiscard]] std::vector<T> read_pixels(std::size_t index, const pixel_read_options& options = {}) const
    {
        std::vector<T> samples(sample_count_of(index, sample_format_of<T>(), options));
        read_pixels(index, std::as_writable_bytes(std::span(samples)), options);
        return samples;
    }

    /// Reads the samples of the image at index into destination, which must hold exactly its number of samples, or
    /// those of options.region, or with options.pitch at least those that the rows of the region span, as values of T,
    /// the type of its sample format.
    /// @throws usage_error when T is another type or destination has another size; and what the other read_pixels()
    ///         throw.
    template <pixel_sample T>
    void read_pixels(std::size_t index, std::span<T> destination, const pixel_read_options& options = {}) const
    {
        check_sample_format(index, sample_format_of<T>());
        read_pixels(index, std::as_writable_bytes(destination), options);
    }

    /// The problems found while opening the unit, in the order they were found: at most limits::max_diagnostics of the
    /// options, 10,000 by default, since a header can hold a million. When there are more, the last diagnostic stands
    /// for itself and all those after it: its message says how many they are, and it has the severity of the most
    /// serious of them and the code of the first of those, without a context.
    [[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;

    /// True unless the unit was opened with read_options::header_only and load_ancillary_data() has not been called.
    [[nodiscard]] bool ancillary_data_loaded() const noexcept;

    /// Loads what read_options::header_only left out: the reader then holds what an open without it gives, its
    /// diagnostics included. It reads the unit from its source again, so it fails like an open, with the options of the
    /// open; the reader is unchanged when it throws. It does nothing when the data are loaded. For a distributed unit,
    /// it keeps the external files that reads of pixels opened, and opens the others through the resolver; the blocks
    /// of a file that cannot be opened are unavailable afterwards, with a diagnostic.
    ///
    /// It changes the reader, so no other function may be called on it at the same time, and the references, pointers
    /// and spans that the reader returned before are invalid afterwards.
    void load_ancillary_data();

private:
    // The number of samples of the image at index, after checking that it has the given sample format, that its data
    // block is available and that its pixel data fit in limits::max_allocation, so that a typed read can allocate them.
    // The typed reads of 1.0 called it, and the library keeps it.
    [[nodiscard]] std::size_t sample_count_of(std::size_t index, sample_format format) const;

    // The number of samples that a typed read of the image at index with options allocates, with the checks of the
    // other overload: those of the region of options, or with a pitch those that its rows span.
    [[nodiscard]] std::size_t sample_count_of(std::size_t index, sample_format format,
                                              const pixel_read_options& options) const;

    // Checks that the image at index has the given sample format, for a typed read into memory of the caller, which
    // allocates nothing.
    void check_sample_format(std::size_t index, sample_format format) const;

    struct state;
    std::unique_ptr<state> state_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace openxisf
