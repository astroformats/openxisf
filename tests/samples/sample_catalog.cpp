// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The units in tests/data/pixinsight, written by PixInsight, the reference implementation of XISF. What the tests
// expect of them is known from how they were made, not from any decoder.

#include "samples/sample_catalog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>

namespace openxisf::test {

namespace {

constexpr std::string_view pixinsight_1_9_4 = "PixInsight 1.9.4-0 build 1695 (XISF module 1.1.3) on Windows 11";

// The pattern image is 37 x 23 pixels, so that the sample count of a block is never a multiple of 16. Channel 0
// increases in storage order (strictly from 16 bits on), channel 1 decreases, channel 2 follows x and the alpha channel
// follows y. Integer formats hold PixInsight's rounding of the values in [0, 1].
constexpr std::string_view group_a_procedure =
    "tests/data/pixinsight/make_group_a.js, in automation mode. PixelMath creates the image with the expressions "
    "(x() + 37*y())/850, 1 - (x() + 37*y())/850, x()/36 and y()/22 for channels 0, 1, 2 and alpha, without rescaling "
    "and with truncation, and ImageWindow.saveAs() saves it with the format hints 'no-compress-data no-checksums "
    "block-alignment 4096 properties fits-keywords'. PixInsight stores a block of up to 3072 bytes, its default "
    "maximum inline block size, in the header; adds its sRGB ICC profile to RGB images; and adds a thumbnail, as the "
    "global preferences of the installation asked.";

constexpr std::string_view nothing_by_hand = "nothing";

// The elements of a header written by PixInsight: one image, with the elements that PixInsight wrote in it, and the
// metadata, with the given number of properties.
std::vector<std::string> image_and_metadata(std::initializer_list<std::string_view> image_children,
                                            int metadata_properties)
{
    std::vector<std::string> paths{"/xisf/Image[1]"};
    for (const std::string_view child : image_children) {
        paths.push_back("/xisf/Image[1]/" + std::string(child));
    }
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= metadata_properties; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

// XISF:CreationTime, CreatorApplication, CreatorModule, CreatorOS, BlockAlignmentSize, MaxInlineBlockSize and
// OutputHints.
std::vector<std::string> group_a_elements(std::initializer_list<std::string_view> image_children)
{
    return image_and_metadata(image_children, 7);
}

// The image and its thumbnail, and the ICC profile of RGB images.
std::vector<std::string> gray_blocks()
{
    return {"/xisf/Image[1]", "/xisf/Image[1]/Thumbnail[1]"};
}

std::vector<std::string> rgb_blocks()
{
    return {"/xisf/Image[1]", "/xisf/Image[1]/ICCProfile[1]", "/xisf/Image[1]/Thumbnail[1]"};
}

constexpr std::string_view group_b_procedure =
    "tests/data/pixinsight/make_group_b.js, in automation mode. PixelMath creates the images of samples A4, A2 and A5 "
    "once each, with the expressions of group A, and ImageWindow.saveAs() saves them with the format hints "
    "'compression-codec <codec> no-checksums block-alignment 4096 properties fits-keywords', at the default level. "
    "PixInsight compresses every block with the codec, without byte shuffling for the ICC profile, the properties and "
    "the thumbnail, and attaches the blocks that it does not embed right after the header, without alignment.";

// The blocks of an RGB sample of group B: the image, its ICC profile, its processing history property, its thumbnail
// and, when PixInsight writes it as a block, the XISF:OutputHints property.
std::vector<std::string> group_b_rgb_blocks(bool output_hints)
{
    std::vector<std::string> paths{"/xisf/Image[1]", "/xisf/Image[1]/ICCProfile[1]", "/xisf/Image[1]/Property[1]",
                                   "/xisf/Image[1]/Thumbnail[1]"};
    if (output_hints) {
        paths.emplace_back("/xisf/Metadata[1]/Property[7]");
    }
    return paths;
}

std::vector<std::string> group_b_gray_blocks()
{
    return {"/xisf/Image[1]", "/xisf/Image[1]/Property[1]", "/xisf/Image[1]/Thumbnail[1]",
            "/xisf/Metadata[1]/Property[7]"};
}

// XISF:CreationTime, CreatorApplication, CreatorModule, CreatorOS, CompressionCodecs, CompressionLevel and
// OutputHints. The thumbnail of zlib samples is small enough to be embedded.
std::vector<std::string> group_b_elements(std::initializer_list<std::string_view> image_children)
{
    return image_and_metadata(image_children, 7);
}

std::vector<std::string> group_b_rgb_elements(bool embedded_thumbnail)
{
    if (embedded_thumbnail) {
        return group_b_elements(
            {"Resolution[1]", "ICCProfile[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"});
    }
    return group_b_elements({"Resolution[1]", "ICCProfile[1]", "Property[1]", "Thumbnail[1]"});
}

constexpr std::string_view group_c_procedure =
    "tests/data/pixinsight/make_group_c.js, in automation mode. PixelMath creates the image of sample A2 once, with "
    "the expression (x() + 37*y())/850, and ImageWindow.saveAs() saves it with the format hints 'block-alignment 4096 "
    "properties fits-keywords' and those of each sample. The image is embedded, and the thumbnail of the global "
    "preferences of the installation is attached. C1 to C3 and C6 were written by PixInsight 1.9.4, and C4, C5 and "
    "C7, with SHA-3 checksums, by PixInsight 1.9.6, whose XISF module writes them.";

// XISF:CreationTime, CreatorApplication, CreatorModule and CreatorOS; BlockAlignmentSize and MaxInlineBlockSize, or
// CompressionCodecs and CompressionLevel; ChecksumAlgorithms and OutputHints.
std::vector<std::string> group_c_elements()
{
    return image_and_metadata({"Data[1]", "Resolution[1]", "Property[1]", "Thumbnail[1]"}, 8);
}

constexpr std::string_view group_d_procedure =
    "tests/data/pixinsight/make_group_d.js, in automation mode. PixelMath creates the image of sample A2 with the "
    "expression (x() + 37*y())/850; View.setPropertyValue() sets the typed properties of the sample on its main view, "
    "storable and permanent; and ImageWindow.saveAs() saves it with the format hints 'no-compression no-checksums "
    "block-alignment 4096 properties fits-keywords'. The image is embedded, and the thumbnail of the global "
    "preferences "
    "of the installation is attached.";

// The properties of sample D3 after the processing history, in the order of their identifiers, as PixInsight writes
// them; true for those with a data block.
struct d3_property
{
    std::string_view id{};
    bool block = false;
};

constexpr std::array<d3_property, 31> d3_properties{{
    {.id = "Test:Boolean"},
    {.id = "Test:C32Vector", .block = true},
    {.id = "Test:C64Vector", .block = true},
    {.id = "Test:EmptyMatrix", .block = true},
    {.id = "Test:EmptyVector", .block = true},
    {.id = "Test:F32Vector", .block = true},
    {.id = "Test:F64Matrix", .block = true},
    {.id = "Test:F64Vector", .block = true},
    {.id = "Test:Float32"},
    {.id = "Test:Float64"},
    {.id = "Test:I16Vector", .block = true},
    {.id = "Test:I32Vector", .block = true},
    {.id = "Test:I64Vector", .block = true},
    {.id = "Test:I8Vector", .block = true},
    {.id = "Test:Int16"},
    {.id = "Test:Int32"},
    {.id = "Test:Int64"},
    {.id = "Test:Int8"},
    {.id = "Test:LargeVector", .block = true},
    {.id = "Test:LongString"},
    {.id = "Test:String"},
    {.id = "Test:TimePoint"},
    {.id = "Test:UI16Matrix", .block = true},
    {.id = "Test:UI16Vector", .block = true},
    {.id = "Test:UI32Vector", .block = true},
    {.id = "Test:UI64Vector", .block = true},
    {.id = "Test:UI8Vector", .block = true},
    {.id = "Test:UInt16"},
    {.id = "Test:UInt32"},
    {.id = "Test:UInt64"},
    {.id = "Test:UInt8"},
}};

// The image with its processing history and the properties of D3, its thumbnail, and the metadata of group A.
std::vector<std::string> d3_elements()
{
    std::vector<std::string> paths{"/xisf/Image[1]", "/xisf/Image[1]/Data[1]", "/xisf/Image[1]/Resolution[1]"};
    for (std::size_t i = 1; i <= d3_properties.size() + 1; ++i) {
        paths.push_back("/xisf/Image[1]/Property[" + std::to_string(i) + "]");
    }
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]");
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= 7; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

// Three images, each with its pixels, embedded when they are small enough, a Resolution element and a property, then
// the metadata of group A.
std::vector<std::string> d4_elements()
{
    std::vector<std::string> paths;
    for (const std::string_view image : {"/xisf/Image[1]", "/xisf/Image[2]", "/xisf/Image[3]"}) {
        paths.emplace_back(image);
        if (image != "/xisf/Image[2]") {
            paths.push_back(std::string(image) + "/Data[1]");
        }
        paths.push_back(std::string(image) + "/Resolution[1]");
        paths.push_back(std::string(image) + "/Property[1]");
    }
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= 7; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

constexpr std::string_view group_d4_procedure =
    "tests/data/pixinsight/make_group_d.js with the argument D4, in automation mode. PixelMath creates each image with "
    "the expressions of group A ((x() + 11*y())/76 for the 11 x 7 image), and a FileFormatInstance of the XISF format "
    "writes them one after the other into one unit, with setOptions(), setImageId(), writeImageProperty() and "
    "writeImage(), and the format hints 'no-compression no-checksums block-alignment 4096 properties fits-keywords'. "
    "Without an image window, PixInsight writes no thumbnail, ICC profile or processing history; it gives each image a "
    "Resolution element.";

constexpr std::string_view group_d1_procedure =
    "tests/data/pixinsight/make_group_d.js with the argument D1, in automation mode. PixelMath creates the image with "
    "the expressions of the colour channels of group A; the script sets the keywords of the image window, its RGB "
    "working space (new RGBColorSystem(2.2, false, Y, x, y) with the values of Adobe RGB (1998) in spec §11.8.2), "
    "the STF of its main view, its resolution, and the properties of its main view, storable and permanent; and "
    "ImageWindow.saveAs() saves it with the format hints 'no-compression no-checksums block-alignment 4096 "
    "max-inline-block-size 64 properties fits-keywords'. PixInsight adds its sRGB ICC profile and the thumbnail of the "
    "global preferences, and writes the image, the ICC profile, the thumbnail and the vector of 800 bytes as attached "
    "blocks, and the vector and matrix of 32 bytes as inline blocks.";

constexpr std::string_view group_d1_by_hand =
    "The keywords OBJECT = 'M31', EXPTIME = 300., HISTORY and COMMENT with their text in the comment, CARD68 (a string "
    "of 68 characters) and LONGSTR (a string of 90 characters); the RGB working space; the STF m = 0.25:0.3:0.35:0.5, "
    "s = 0.01:0.02:0.03:0, h = 0.9:0.95:1:1, l = 0 and r = 1; a resolution of 120 by 100 pixels per centimetre; and "
    "the properties Observation:Object:Name = M31, Observation:Center:RA = 10.684708, Observation:Center:Dec = "
    "41.26875, Observation:Time:Start = 2026-09-20T23:15:00Z, Instrument:ExposureTime = 300, Instrument:Camera:Name = "
    "Synthetic camera, Instrument:Telescope:FocalLength = 0.53, Instrument:Sensor:Temperature = -10, Test:SmallVector "
    "= [1, 2, 3, 4], Test:SmallMatrix = [[1, 2], [3, 4]] and Test:AttachedVector = [0, 0.25, ..., 24.75].";

// The image with its keywords, working space, display function, resolution, ICC profile, properties and thumbnail,
// then the metadata of group A.
std::vector<std::string> d1_elements()
{
    std::vector<std::string> paths{"/xisf/Image[1]"};
    for (int i = 1; i <= 6; ++i) {
        paths.push_back("/xisf/Image[1]/FITSKeyword[" + std::to_string(i) + "]");
    }
    for (const std::string_view child :
         {"RGBWorkingSpace[1]", "DisplayFunction[1]", "Resolution[1]", "ICCProfile[1]"}) {
        paths.push_back("/xisf/Image[1]/" + std::string(child));
    }
    for (int i = 1; i <= 12; ++i) {
        paths.push_back("/xisf/Image[1]/Property[" + std::to_string(i) + "]");
    }
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]");
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= 7; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

constexpr std::string_view group_d2_procedure =
    "tests/data/pixinsight/make_group_d.js with the argument D2, in automation mode. PixelMath creates a 64 x 48 Gray "
    "UInt16 image with the expression (x() + 64*y())/3071, and a FileFormatInstance of the XISF format writes it with "
    "setOptions() (an ImageDescription of 16-bit integers with imageType 4, Light), the colorFilterArray ['RGGB', 2, "
    "2, 'RGGB Bayer filter'] and the keywords of a camera, and the format hints 'no-compression no-checksums "
    "block-alignment 4096 properties fits-keywords'. A synthetic frame stands for a raw frame opened as a CFA image: "
    "the XISF module writes the same elements for one, and a synthetic frame holds no data of a real camera.";

constexpr std::string_view group_d2_by_hand =
    "The colour filter array, the image type, and the keywords INSTRUME = 'Synthetic CFA camera', BAYERPAT = 'RGGB', "
    "XBAYROFF = 0, YBAYROFF = 0, EXPTIME = 120., GAIN = 120, XPIXSZ = 2.4, YPIXSZ = 2.4 and CCD-TEMP = -10.";

// The image with its keywords, colour filter array and resolution, then the metadata of group A.
std::vector<std::string> d2_elements()
{
    std::vector<std::string> paths{"/xisf/Image[1]"};
    for (int i = 1; i <= 9; ++i) {
        paths.push_back("/xisf/Image[1]/FITSKeyword[" + std::to_string(i) + "]");
    }
    paths.emplace_back("/xisf/Image[1]/ColorFilterArray[1]");
    paths.emplace_back("/xisf/Image[1]/Resolution[1]");
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= 7; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

std::vector<std::string> d3_blocks()
{
    std::vector<std::string> paths{"/xisf/Image[1]"};
    for (std::size_t i = 0; i < d3_properties.size(); ++i) {
        if (d3_properties[i].block) {
            paths.push_back("/xisf/Image[1]/Property[" + std::to_string(i + 2) + "]");
        }
    }
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]");
    return paths;
}

constexpr std::string_view pixinsight_1_9_5 = "PixInsight 1.9.5-0 build 1706 (XISF module 1.1.3) on Windows 11";

constexpr std::string_view pixinsight_1_9_6 = "PixInsight 1.9.6-0 build 1713 (XISF module 1.2.0) on Windows 11";

// The XISF module of PixInsight 1.9.6 (spec Revision 2): a compressed block is divided into subblocks of 4 MiB, a
// subblock that its codec does not make shorter is stored as it is, with equal sizes (spec §10.6.1), and a block none
// of whose subblocks gets shorter is written without compression.
constexpr std::string_view group_e_procedure =
    "tests/data/pixinsight/make_group_e.js, in automation mode, with PixInsight 1.9.6. PixelMath creates a 2048 x 1025 "
    "Gray UInt16 image with the expression iif(y() >= 1023, random(), x()/2047), a ramp along x whose last two rows "
    "(4096 samples) are noise, and a 64 x 64 image of random(), without rescaling and with truncation, and "
    "ImageWindow.saveAs() saves them with the format hints 'compression-codec <codec> no-checksums block-alignment "
    "4096 properties fits-keywords', with 'checksums sha256' for E4. PixInsight divides the block of the ramp into "
    "a subblock of 4 MiB, compressed, and one of 4096 bytes, which no codec makes shorter and which it stores as it "
    "is: the high bytes of the noise with byte shuffling, which puts the low bytes of every sample first, or the "
    "last 2048 samples without it. It writes the block of the noise image without compression. It compresses the "
    "thumbnail and the processing history property as well, embeds the thumbnail of the ramp, whose compressed "
    "form fits, and attaches the blocks that it does not embed right after the header, without alignment.";

// The blocks of a sample of group E: the image, its processing history property, its thumbnail, and the
// XISF:OutputHints property, which PixInsight writes as a block after the metadata properties it generates.
std::vector<std::string> group_e_blocks(int output_hints)
{
    return {"/xisf/Image[1]", "/xisf/Image[1]/Property[1]", "/xisf/Image[1]/Thumbnail[1]",
            "/xisf/Metadata[1]/Property[" + std::to_string(output_hints) + "]"};
}

constexpr std::string_view group_d5_procedure =
    "tests/data/pixinsight/make_group_d5.js, in automation mode. PixelMath creates a 400 x 300 Gray UInt16 image with "
    "the expression (x() + y())/1398; the script sets on its main view the properties that ImageSolver 6.5.0 sets for "
    "a solution: the first layer of a Gnomonic projection about M42 and its native coordinates, the projective "
    "transformations (Math.homography() of the control points), the control points, the creation time, creator and "
    "PixInsight's generation parameters; ImageWindow.regenerateAstrometricSolution() makes the core generate the "
    "distortion models, and ImageWindow.saveAs() saves it with the format hint 'compression-codec zstd+sh'. The "
    "control points are synthetic: random image points of a known model with a radial distortion and a wave, and "
    "their celestial coordinates. PixInsight adds the WCS keywords TIMESYS, RA, OBJCTRA, DEC and OBJCTDEC, the "
    "Observation:Center and reference system properties, its grids of the transformations "
    "(PCL:AstrometricSolution:Grid), and the thumbnail of the global preferences. d5-reference.csv holds PixInsight's "
    "coordinates for the solutions, evaluated on a copy of each on a 2 x 2 image, where its grids cover no point "
    "evaluated.";

// The image of a sample of group D5, its keywords, resolution, properties and thumbnail, then the metadata.
std::vector<std::string> d5_elements(int properties)
{
    std::vector<std::string> paths{"/xisf/Image[1]", "/xisf/Image[1]/Data[1]"};
    for (int i = 1; i <= 5; ++i) {
        paths.push_back("/xisf/Image[1]/FITSKeyword[" + std::to_string(i) + "]");
    }
    paths.emplace_back("/xisf/Image[1]/Resolution[1]");
    for (int i = 1; i <= properties; ++i) {
        paths.push_back("/xisf/Image[1]/Property[" + std::to_string(i) + "]");
    }
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]");
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]/Data[1]");
    paths.emplace_back("/xisf/Metadata[1]");
    for (int i = 1; i <= 7; ++i) {
        paths.push_back("/xisf/Metadata[1]/Property[" + std::to_string(i) + "]");
    }
    return paths;
}

// The blocks of a sample of group D5: its pixels, the properties that are not among those written in the header, and
// its thumbnail.
std::vector<std::string> d5_blocks(int properties, std::initializer_list<int> in_header)
{
    std::vector<std::string> paths{"/xisf/Image[1]"};
    for (int i = 1; i <= properties; ++i) {
        if (std::ranges::find(in_header, i) == in_header.end()) {
            paths.push_back("/xisf/Image[1]/Property[" + std::to_string(i) + "]");
        }
    }
    paths.emplace_back("/xisf/Image[1]/Thumbnail[1]");
    return paths;
}

const std::vector<sample>& catalog()
{
    static const std::vector<sample> samples{
        {.id = "A1",
         .file = "a1-gray-u8.xisf",
         .content = "The pattern image, Gray, UInt8, in an embedded block.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Data[1]", "Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = gray_blocks()},
        {.id = "A2",
         .file = "a2-gray-u16.xisf",
         .content = "The pattern image, Gray, UInt16, in an embedded block.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Data[1]", "Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = gray_blocks()},
        {.id = "A3",
         .file = "a3-gray-u32.xisf",
         .content = "The pattern image, Gray, UInt32, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = gray_blocks()},
        {.id = "A4",
         .file = "a4-rgb-f32.xisf",
         .content = "The pattern image, RGB, Float32 with bounds 0:1, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Resolution[1]", "ICCProfile[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = rgb_blocks()},
        {.id = "A5",
         .file = "a5-gray-f64.xisf",
         .content = "The pattern image, Gray, Float64 with bounds 0:1, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = gray_blocks()},
        {.id = "A6",
         .file = "a6-rgba-u16.xisf",
         .content = "The pattern image, RGB with an alpha channel, UInt16, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_a_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_a_elements({"Resolution[1]", "ICCProfile[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = rgb_blocks()},
        {.id = "B1",
         .file = "b1-rgb-f32-zlib.xisf",
         .content = "The image of A4 compressed with zlib, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(true),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B2",
         .file = "b2-rgb-f32-zlib-sh.xisf",
         .content = "The image of A4 compressed with zlib+sh (4-byte items), attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(true),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B3",
         .file = "b3-rgb-f32-lz4.xisf",
         .content = "The image of A4 compressed with lz4, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(false)},
        {.id = "B4",
         .file = "b4-rgb-f32-lz4-sh.xisf",
         .content = "The image of A4 compressed with lz4+sh (4-byte items), attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B5",
         .file = "b5-rgb-f32-lz4hc.xisf",
         .content = "The image of A4 compressed with lz4hc, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B6",
         .file = "b6-rgb-f32-lz4hc-sh.xisf",
         .content = "The image of A4 compressed with lz4hc+sh (4-byte items), attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B7",
         .file = "b7-rgb-f32-zstd.xisf",
         .content = "The image of A4 compressed with zstd, attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B8",
         .file = "b8-rgb-f32-zstd-sh.xisf",
         .content = "The image of A4 compressed with zstd+sh (4-byte items), attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
        {.id = "B9",
         .file = "b9-gray-u16-zstd-sh.xisf",
         .content = "The image of A2 compressed with zstd+sh (2-byte items), embedded.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Data[1]", "Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = group_b_gray_blocks()},
        {.id = "B10",
         .file = "b10-gray-f64-zlib-sh.xisf",
         .content = "The image of A5 compressed with zlib+sh (8-byte items), attached.",
         .producer = pixinsight_1_9_4,
         .procedure = group_b_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"}),
         .blocks = group_b_gray_blocks()},
        // The XISF module of PixInsight 1.9.4 writes SHA-1, SHA-256 and SHA-512 checksums only; that of 1.9.6 writes
        // SHA3-256 and SHA3-512 too, which C4, C5 and C7 hold.
        {.id = "C1",
         .file = "c1-gray-u16-sha1.xisf",
         .content = "The image of A2 with SHA-1 checksums, as sha1, on the Data element of the embedded image and on "
                    "the attached thumbnail.",
         .producer = pixinsight_1_9_4,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = gray_blocks()},
        {.id = "C2",
         .file = "c2-gray-u16-sha256.xisf",
         .content = "The image of A2 with SHA-256 checksums, as sha256.",
         .producer = pixinsight_1_9_4,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = gray_blocks()},
        {.id = "C3",
         .file = "c3-gray-u16-sha512.xisf",
         .content = "The image of A2 with SHA-512 checksums, as sha512.",
         .producer = pixinsight_1_9_4,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = gray_blocks()},
        {.id = "C4",
         .file = "c4-gray-u16-sha3-256.xisf",
         .content = "The image of A2 with SHA3-256 checksums, as sha3-256, written by PixInsight 1.9.6.",
         .producer = pixinsight_1_9_6,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = gray_blocks(),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "C5",
         .file = "c5-gray-u16-sha3-512.xisf",
         .content = "The image of A2 with SHA3-512 checksums, as sha3-512, written by PixInsight 1.9.6.",
         .producer = pixinsight_1_9_6,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = gray_blocks(),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "C6",
         .file = "c6-gray-u16-zstd-sh-sha256.xisf",
         .content = "The image of A2 compressed with zstd+sh (2-byte items), and a SHA-256 checksum of the compressed "
                    "data, both on the Data element of the embedded image. PixInsight also compresses the thumbnail "
                    "(zstd, attached right after the header, not aligned) and the processing history property (zstd, "
                    "inline), and writes the XISF:OutputHints property as an inline block, all with SHA-256 "
                    "checksums.",
         .producer = pixinsight_1_9_4,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = {"/xisf/Image[1]", "/xisf/Image[1]/Property[1]", "/xisf/Image[1]/Thumbnail[1]",
                    "/xisf/Metadata[1]/Property[8]"}},
        {.id = "C7",
         .file = "c7-gray-u16-zstd-sh-sha3-256.xisf",
         .content = "The image of A2 compressed with zstd+sh (2-byte items), and a SHA3-256 checksum of the compressed "
                    "data, as C6 with sha3-256 digests on the same blocks, written by PixInsight 1.9.6.",
         .producer = pixinsight_1_9_6,
         .procedure = group_c_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_c_elements(),
         .blocks = {"/xisf/Image[1]", "/xisf/Image[1]/Property[1]", "/xisf/Image[1]/Thumbnail[1]",
                    "/xisf/Metadata[1]/Property[8]"},
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        // Group E: what the XISF module of PixInsight 1.9.6 writes under Revision 2 of the specification.
        {.id = "E1",
         .file = "e1-gray-u16-zstd-sh-stored.xisf",
         .content =
             "The ramp image compressed with zstd+sh (2-byte items) in two subblocks, of 4 MiB and of 4096 bytes, "
             "the second stored as it is: the high bytes of the 4096 noise samples, which byte shuffling puts "
             "last. Attached.",
         .producer = pixinsight_1_9_6,
         .procedure = group_e_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"}),
         .blocks = group_e_blocks(7),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "E2",
         .file = "e2-gray-u16-lz4-sh-stored.xisf",
         .content = "The ramp image compressed with lz4+sh (2-byte items) in two subblocks, the second of 4096 bytes "
                    "stored as it is, as in E1. Attached.",
         .producer = pixinsight_1_9_6,
         .procedure = group_e_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"}),
         .blocks = group_e_blocks(7),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "E3",
         .file = "e3-gray-u16-zstd-stored.xisf",
         .content = "The ramp image compressed with zstd, without byte shuffling, in two subblocks, the second of 4096 "
                    "bytes stored as it is: the last 2048 samples, of the noise. Attached.",
         .producer = pixinsight_1_9_6,
         .procedure = group_e_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"}),
         .blocks = group_e_blocks(7),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "E4",
         .file = "e4-gray-u16-zstd-sh-stored-sha256.xisf",
         .content = "The image of E1, with SHA-256 checksums of the stored bytes, the stored subblock included, as "
                    "spec §10.6.1 asks. Attached.",
         .producer = pixinsight_1_9_6,
         .procedure = group_e_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = image_and_metadata({"Resolution[1]", "Property[1]", "Thumbnail[1]", "Thumbnail[1]/Data[1]"}, 8),
         .blocks = group_e_blocks(8),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        {.id = "E5",
         .file = "e5-gray-u16-zstd-sh-noise.xisf",
         .content = "The 64 x 64 image of noise, saved with zstd+sh: no codec makes its block shorter, so PixInsight "
                    "writes it without compression, attached, and names zstd in XISF:CompressionCodecs for the "
                    "thumbnail and the processing history, which it compresses.",
         .producer = pixinsight_1_9_6,
         .procedure = group_e_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_elements({"Resolution[1]", "Property[1]", "Thumbnail[1]"}),
         .blocks = group_e_blocks(7),
         .application = "PixInsight 1.9.6",
         .module = "XISF module version 1.2.0",
         .creation_time_as_string = false},
        // D6 and D7 do not exist: the XISF module of PixInsight has no format hint for normal storage,
        // big-endian blocks or CIE L*a*b*, cannot write a complex image, and PixInsight cannot sign XISF units.
        // Constructed units cover them (tests/conformance/image.cpp, byte_order.cpp and signature.cpp).
        {.id = "D1",
         .file = "d1-rich-rgb.xisf",
         .content = "The pattern image in RGB and UInt16 with what describes an image: six FITS keywords (HISTORY and "
                    "COMMENT, quoted strings, one of 68 characters and one of 90), the RGB working space of Adobe RGB "
                    "(1998), a display function, a resolution of 120 by 100 pixels per centimetre, PixInsight's sRGB "
                    "ICC profile (3,024 bytes), eight Observation and Instrument properties, the processing history, "
                    "two vectors and a matrix of Float64, and a 400 x 248 RGB UInt8 thumbnail. PixInsight writes no "
                    "name for the working space or the display function, and the chromaticities and luminance "
                    "coefficients of the working space with the 17 digits of the 32-bit floating point values it "
                    "holds them in.",
         .producer = pixinsight_1_9_4,
         .procedure = group_d1_procedure,
         .set_by_hand = group_d1_by_hand,
         .elements = d1_elements(),
         .blocks = {"/xisf/Image[1]", "/xisf/Image[1]/ICCProfile[1]", "/xisf/Image[1]/Property[10]",
                    "/xisf/Image[1]/Property[11]", "/xisf/Image[1]/Property[12]", "/xisf/Image[1]/Thumbnail[1]"}},
        {.id = "D2",
         .file = "d2-cfa-raw.xisf",
         .content = "A synthetic 64 x 48 Bayer frame, Gray, UInt16, whose samples increase in storage order, attached, "
                    "with the image type Light, nine camera keywords, the colour filter array RGGB of 2 x 2 named "
                    "'RGGB Bayer filter', and a resolution of 72 pixels per inch. Without an image window, PixInsight "
                    "writes no thumbnail, ICC profile or processing history.",
         .producer = pixinsight_1_9_4,
         .procedure = group_d2_procedure,
         .set_by_hand = group_d2_by_hand,
         .elements = d2_elements(),
         .blocks = {"/xisf/Image[1]"}},
        {.id = "D3",
         .file = "d3-typed-properties.xisf",
         .content = "The image of A2 with 31 typed properties: the extremes of each integer type, Float32 0.1 and "
                    "Float64 1e-300, a String with two spaces at both ends and characters beyond ASCII, a String of "
                    "10,000 characters, a TimePoint with milliseconds, each vector type with the extremes of its "
                    "elements, two complex vectors, an empty vector and an empty matrix, a vector of 100,000 Float32 "
                    "elements (the only property in an attached block), and two matrices. PixInsight writes them in "
                    "the order of their identifiers, the strings as character data and the other vectors and matrices "
                    "as inline blocks. It cannot write complex scalars, format specifiers or comments.",
         .producer = pixinsight_1_9_4,
         .procedure = group_d_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = d3_elements(),
         .blocks = d3_blocks()},
        {.id = "D4",
         .file = "d4-multi-image.xisf",
         .content =
             "Three images in one unit: the image of A2 with the id 'first' and the UInt16 property Test:First = "
             "1, embedded; the image of A4 with the id 'second' and the String property Test:Second = 'second "
             "image', attached; and an 11 x 7 Gray UInt8 image whose samples increase in storage order, with the "
             "id 'third' and the Float64 property Test:Third = 3.5, embedded.",
         .producer = pixinsight_1_9_4,
         .procedure = group_d4_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = d4_elements(),
         .blocks = {"/xisf/Image[1]", "/xisf/Image[2]", "/xisf/Image[3]"}},
        {.id = "D5",
         .file = "d5-astrometry.xisf",
         .content = "A 400 x 300 Gray UInt16 image, embedded, with an astrometric solution as ImageSolver writes one "
                    "with its default distortion correction: a Gnomonic projection, projective transformations, and "
                    "distortion models of one Global term of thin plate splines of order 2 in each direction, whose Y "
                    "components share the nodes of the X ones; 1000 control points.",
         .producer = pixinsight_1_9_5,
         .procedure = group_d5_procedure,
         .set_by_hand = "The parameters of the model of the control points, and the generation parameters engine "
                        "DDM, RBFType DDMThinPlateSpline, order 2, smoothness 0.005, 4000 spline points, simplifiers "
                        "rejecting 10%.",
         .elements = d5_elements(53),
         .blocks = d5_blocks(53, {2,  5,  6,  7,  8,  9,  14, 15, 16, 17, 22, 23, 24, 26, 32,
                                  33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 49}),
         .application = "PixInsight 1.9.5"},
        {.id = "D5L",
         .file = "d5-astrometry-local.xisf",
         .content = "The image of D5 with a solution of recursive surface splines of order 4 (VariableOrder): "
                    "distortion models of 28 and 37 Local terms and a Fallback term, whose Y components share the "
                    "nodes of the X ones; 800 control points.",
         .producer = pixinsight_1_9_5,
         .procedure = group_d5_procedure,
         .set_by_hand = "As D5, with the engine Recursive, RBFType VariableOrder, order 4, and patches of at most 300 "
                        "nodes, buckets of 64 points and 250 coarse points, a radius factor of 1.5.",
         .elements = d5_elements(74),
         .blocks = d5_blocks(74, {2,  6,  7,  8,  9,  10, 11, 23, 24, 25, 26, 27, 39, 40, 41, 43, 49, 50,
                                  51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 70}),
         .application = "PixInsight 1.9.5"},
        {.id = "D5M",
         .file = "d5-astrometry-multiquadric.xisf",
         .content = "The image of D5 with a solution of multiquadric splines of order 2: one Global term in each "
                    "direction with shape parameters, whose Y component has nodes, a normalization and a shape "
                    "parameter of its own in the projection-to-image direction; 1000 control points.",
         .producer = pixinsight_1_9_5,
         .procedure = group_d5_procedure,
         .set_by_hand = "As D5, with the engine DDM and RBFType DDMMultiquadric.",
         .elements = d5_elements(59),
         .blocks = d5_blocks(59, {2,  5,  6,  7,  8,  9,  13, 15, 16, 17, 18, 19, 23, 27, 28, 29, 30,
                                  32, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 55}),
         .application = "PixInsight 1.9.5"},
    };
    return samples;
}

constexpr std::string_view large_l1_procedure =
    "tests/data/pixinsight/make_large_l1.js, in automation mode. PixelMath creates the image with the expressions "
    "x()/21999, y()/16999 and (x() + y())/38998, without rescaling and with truncation, and ImageWindow.saveAs() saves "
    "it with the format hints 'compression-codec zlib no-checksums block-alignment 4096 properties fits-keywords'. "
    "PixInsight divides the image into a subblock of 4,294,967,294 bytes, which it stores as it is, with equal sizes, "
    "and one of the remaining 193,032,706 bytes, compressed with zlib.";

} // namespace

const std::vector<sample>& large_samples()
{
    static const std::vector<sample> samples{
        {.id = "L1",
         .file = "l1-rgb-f32-zlib.xisf",
         .content = "An RGB Float32 image of 22000 x 17000 pixels (4,488,000,000 bytes) compressed with zlib in two "
                    "subblocks, attached; 4.45 GB.",
         .producer = pixinsight_1_9_4,
         .procedure = large_l1_procedure,
         .set_by_hand = nothing_by_hand,
         .elements = group_b_rgb_elements(false),
         .blocks = group_b_rgb_blocks(true)},
    };
    return samples;
}

const std::vector<sample>& all_samples()
{
    return catalog();
}

std::vector<sample> samples_of_group(char group)
{
    std::vector<sample> found;
    std::ranges::copy_if(catalog(), std::back_inserter(found),
                         [group](const sample& unit) { return unit.id.front() == group; });
    return found;
}

const sample& sample_by_id(std::string_view id)
{
    for (const sample& unit : catalog()) {
        if (unit.id == id) {
            return unit;
        }
    }
    throw std::logic_error("no sample " + std::string(id));
}

std::vector<diagnostic> unexpected_diagnostics(std::span<const diagnostic> diagnostics)
{
    std::vector<diagnostic> others;
    for (const diagnostic& entry : diagnostics) {
        if (entry.severity != severity::info || entry.code != errc::reserved_property_type ||
            entry.context.element != creation_time_path) {
            others.push_back(entry);
        }
    }
    return others;
}

std::string sample_path(const sample& unit)
{
    return std::string(OPENXISF_TEST_DATA_DIR) + "/pixinsight/" + std::string(unit.file);
}

void PrintTo(const sample& unit, std::ostream* output)
{
    *output << unit.id;
}

} // namespace openxisf::test
