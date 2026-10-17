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

#include <cstddef>
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
    /// Read the header alone: leave the ancillary data in data blocks unloaded, so that nothing but the header is read,
    /// and for a distributed unit the block indexes of its data blocks files. ICC profiles and the pixels of thumbnails
    /// stay empty, and the properties and tables with values in data blocks are left out, until
    /// reader::load_ancillary_data() loads them; the problems of those data are found then. Their identifiers are
    /// known: another property or table of the object with one of them is refused, as a full open refuses it, and a
    /// mandatory metadata property left out is not reported missing. The model is not complete until then, so the
    /// writer would lose those data, or refuse a thumbnail without pixels (writer).
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
    /// from the source plus, for a compressed image, the bytes decompressed.
    progress_function progress{};
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
/// unavailable, with a diagnostic; the files stay open as long as the reader exists. It describes each image and what
/// is associated with it, and loads the ancillary data in data blocks, within limits::max_ancillary_data: ICC
/// profiles, the pixels of thumbnails, and the values of properties and table cells (read_options::header_only defers
/// them). Pixel data are read on request, with read_pixels(). Problems confined to one object of the unit do not fail
/// it: the object is unavailable, and a diagnostic says why (spec §7). An external file whose source fails with
/// io_error is such a problem, while its block index is read, for the blocks in the file, and while ancillary data are
/// loaded from it, for the block being loaded; an io_error of the source of the unit fails the open. A signed unit is
/// read like any other; its signature is returned, not verified (signature()).
///
/// The reader refuses what affects safety or the meaning of the data, and reads departures from the specification that
/// are unambiguous and harmless with a warning: among others, hexadecimal digits in uppercase, codec and checksum
/// algorithm names in another case than the specification's, FITS keyword names padded with trailing spaces, which are
/// read without them, values of the Image namespace that spec §11.5.3.5 forbids, such as a frame number of 0, which are
/// kept, a property or table that an object contains and names, or names twice, which it has once, and characters that
/// XML 1.0 does not allow, such as control characters other than tab, line feed and carriage return, which are kept as
/// written. A compressed block or subblock whose compressed size equals its uncompressed size is read as stored,
/// without decompression, as PixInsight writes and reads a subblock that its codec did not make smaller
/// (write_options::subblock_size); the specification does not say so, so codec output of exactly the size of its
/// input, from another encoder, is not decoded.
///
/// The reader owns its source, so a file stays open as long as its reader exists. Its const member functions can be
/// called from any number of threads at once, pixel reads included; load_ancillary_data(), the only function that
/// changes it, needs exclusive access. It can be moved, and a moved-from reader can only be destroyed or assigned to.
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

    /// Reads the pixel data of the image at index: its samples in native byte order, in the storage model of
    /// options.storage, or in that of the image. The data are verified against their checksum and decompressed first.
    /// @throws usage_error when index is out of range.
    /// @throws integrity_error when the data fail their checksum or do not decompress to their size.
    /// @throws unsupported_error, invalid_data_error or limit_error when the data block of the image is unavailable, or
    ///         larger than limits::max_allocation, with the code of the error diagnostic about it; and io_error or
    ///         unsupported_error, with that code, when the resolver could not open the external file of the block.
    /// @throws cancelled_error when options.progress returns false.
    /// @throws io_error, or what a custom source throws, when the source fails.
    [[nodiscard]] std::vector<std::byte> read_pixels(std::size_t index, const pixel_read_options& options = {}) const;

    /// Reads the pixel data of the image at index into destination, which must have exactly image(index).data_size()
    /// bytes, with the exceptions of the other read_pixels(). Data that keep their storage model are read, or
    /// decompressed, straight into destination, so limits::max_allocation applies only to what the read allocates
    /// besides: the stored bytes of compressed data, and for byte-shuffled data the buffer of their size that they are
    /// unshuffled from. When it throws, destination may hold part of the data, but never data that failed their
    /// checksum.
    /// @throws usage_error when destination has another size.
    void read_pixels(std::size_t index, std::span<std::byte> destination, const pixel_read_options& options = {}) const;

    /// Reads the samples of the image at index as values of T, which must be the type of its sample format
    /// (sample_format_of<T>()): std::uint16_t for a UInt16 image, std::complex<float> for a Complex32 image.
    /// @throws usage_error when T is another type; and what the other read_pixels() throw.
    template <pixel_sample T>
    [[nodiscard]] std::vector<T> read_pixels(std::size_t index, const pixel_read_options& options = {}) const
    {
        std::vector<T> samples(sample_count_of(index, sample_format_of<T>()));
        read_pixels(index, std::as_writable_bytes(std::span(samples)), options);
        return samples;
    }

    /// Reads the samples of the image at index into destination, which must hold exactly its number of samples, as
    /// values of T, the type of its sample format.
    /// @throws usage_error when T is another type or destination has another size; and what the other read_pixels()
    ///         throw.
    template <pixel_sample T>
    void read_pixels(std::size_t index, std::span<T> destination, const pixel_read_options& options = {}) const
    {
        check_sample_format(index, sample_format_of<T>());
        read_pixels(index, std::as_writable_bytes(destination), options);
    }

    /// The problems found while opening the unit, in the order they were found: at most 10,000, since a header can hold
    /// a million. When there are more, the last diagnostic stands for itself and all those after it: its message says
    /// how many they are, and it has the severity of the most serious of them and the code of the first of those,
    /// without a context.
    [[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;

    /// True unless the unit was opened with read_options::header_only and load_ancillary_data() has not been called.
    [[nodiscard]] bool ancillary_data_loaded() const noexcept;

    /// Loads what read_options::header_only left out: the reader then holds what an open without it gives, its
    /// diagnostics included. It reads the unit from its source again, so it fails like an open, with the options of the
    /// open; the reader is unchanged when it throws. It does nothing when the data are loaded. For a distributed unit,
    /// it opens the external files again, through the resolver, before it closes those of the open, so up to twice
    /// limits::max_external_files of them are open meanwhile; the blocks of a file that cannot be opened again are
    /// unavailable afterwards, with a diagnostic.
    ///
    /// It changes the reader, so no other function may be called on it at the same time, and the references, pointers
    /// and spans that the reader returned before are invalid afterwards.
    void load_ancillary_data();

private:
    // The number of samples of the image at index, after checking that it has the given sample format, that its data
    // block is available and that its pixel data fit in limits::max_allocation, so that a typed read can allocate them.
    [[nodiscard]] std::size_t sample_count_of(std::size_t index, sample_format format) const;

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
