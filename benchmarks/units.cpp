// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Benchmarks of whole units, read from and written to memory or a file: opening a unit with a rich header, reading
// pixel data into memory of the caller against a raw read of the same bytes, converting between the storage models,
// reading blocks with checksums, and reading and writing a 100 MiB frame divided into subblocks, with its decompression
// and unshuffling alone. Then regions of interest: a tile of the frame and of a colour image, the same tile of the 32
// frames of a stacking, and an editor that fills a frame tile by tile. Files are read from the cache of the system and
// around it, as a file not yet in memory is read. In a build with oneTBB, the conversions and the frame run in task
// arenas of several sizes, and their throughput counts wall time, not the processor time of the calling thread.

#include <openxisf/editor.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "benchmarks.h"
#include "codec/compressed_block.h"
#include "codec/shuffle.h"
#include "model/pixel_layout.h"
#include "uncached_file.h"

#include <benchmark/benchmark.h>

#if defined(OPENXISF_WITH_TBB)
#include <oneapi/tbb/task_arena.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace openxisf::bench {

namespace {

constexpr std::uint64_t mib = std::uint64_t{1} << 20;

// The frame: 5120 × 5120 Float32 samples, 100 MiB.
constexpr std::size_t frame_side = 5120;

// The colour image to convert: 3072 × 3072 pixels of three Float32 samples, 108 MiB.
constexpr std::size_t rgb_side = 3072;

const std::vector<std::byte>& frame()
{
    static const std::vector<std::byte> pixels = sky_image(frame_side, frame_side, 1, samples::float32);
    return pixels;
}

// The task arenas in which the parallel work runs: their numbers of threads.
std::vector<int> arena_sizes()
{
#if defined(OPENXISF_WITH_TBB)
    return {1, 8, 16};
#else
    return {1};
#endif
}

// Runs body in a oneTBB task arena of the given threads, where the library does its parallel work, or on this thread in
// a build without oneTBB.
template <typename Body> void in_arena(int threads, Body&& body)
{
#if defined(OPENXISF_WITH_TBB)
    oneapi::tbb::task_arena arena(threads);
    arena.execute(std::forward<Body>(body));
#else
    (void)threads;
    std::forward<Body>(body)();
#endif
}

void count_bytes(benchmark::State& state, std::uint64_t size)
{
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

image_info gray_image(std::size_t width, std::size_t height, sample_format format)
{
    image_info info;
    info.geometry = {.dimensions = {width, height}, .channels = 1};
    info.sample_format = format;
    if (format == sample_format::float32) {
        info.bounds = bounds{.lower = 0.0, .upper = 1.0};
    }
    return info;
}

// How the pixel data of a unit are written.
struct encoding
{
    std::string name{};
    std::optional<openxisf::codec> codec{};
    std::uint64_t subblock_size = 0;
    std::optional<checksum_algorithm> checksum{};
    // Shuffled when compressed, unless false.
    bool byte_shuffle = true;
};

write_options options_of(const encoding& how)
{
    return {.creator_application = "OpenXISF benchmarks 1.0",
            .codec = how.codec,
            .byte_shuffle = how.codec.has_value() && how.byte_shuffle,
            .subblock_size = how.subblock_size,
            .checksum = how.checksum};
}

std::vector<std::byte> write_unit(const image_info& info, std::span<const std::byte> pixels, const encoding& how)
{
    writer output(options_of(how));
    output.add_image(info, pixels);
    memory_sink sink;
    output.save(sink);
    return sink.release();
}

// A sink that keeps no bytes, so that a write measures the encoding of the unit, not the growth of a buffer.
class discarding_sink final : public output_sink
{
public:
    void write(std::span<const std::byte> data) override
    {
        position_ += data.size();
    }

    std::uint64_t position() const override
    {
        return position_;
    }

    bool can_rewrite() const override
    {
        return true;
    }

    void rewrite(std::uint64_t /*offset*/, std::span<const std::byte> /*data*/) override {}

private:
    std::uint64_t position_ = 0;
};

// A file in the directory of temporary files, removed when the program ends.
class temporary_file
{
public:
    temporary_file(const std::string& name, std::span<const std::byte> content)
        : path_(std::filesystem::temp_directory_path() / name)
    {
        std::ofstream output(path_, std::ios::binary);
        output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        output.close();
        // On the disk before any benchmark reads it, so that the system does not write it back meanwhile.
        file_device(path()).flush();
    }

    temporary_file(const temporary_file&) = delete;
    temporary_file& operator=(const temporary_file&) = delete;
    temporary_file(temporary_file&&) = delete;
    temporary_file& operator=(temporary_file&&) = delete;

    ~temporary_file()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    // The path in UTF-8, as the library takes it.
    [[nodiscard]] std::string path() const
    {
        const std::u8string text = path_.u8string();
        return {text.begin(), text.end()};
    }

    [[nodiscard]] const std::filesystem::path& location() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Where the bytes of a unit come from: memory of the program, a file that the cache of the system holds, or a file
// read around that cache, which stands for a file that is not in memory yet.
enum class medium
{
    memory,
    file,
    cold,
};

const std::array media{std::pair{medium::memory, "memory"}, std::pair{medium::file, "file"},
                       std::pair{medium::cold, "cold"}};

// A unit held in memory and in a file of its own.
class stored_unit
{
public:
    stored_unit(const std::string& file_name, std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), file_(file_name, bytes_)
    {}

    [[nodiscard]] std::unique_ptr<input_source> open(medium where) const
    {
        if (where == medium::memory) {
            return std::make_unique<memory_source>(std::span(bytes_));
        }
        if (where == medium::file) {
            return std::make_unique<file_source>(file_.path());
        }
        return open_uncached(file_.location());
    }

private:
    std::vector<std::byte> bytes_;
    temporary_file file_;
};

// The uncompressed unit of the frame.
const stored_unit& frame_unit()
{
    static const stored_unit unit("openxisf-benchmark-frame.xisf",
                                  write_unit(gray_image(frame_side, frame_side, sample_format::float32), frame(), {}));
    return unit;
}

// ---------------------------------------------------------------------------------------------------------------------
// Opening a unit

// A unit whose header has 3 × count properties, a third of them vectors in inline blocks, and count FITS keywords.
std::vector<std::byte> rich_unit(int count)
{
    image_info info = gray_image(64, 64, sample_format::uint16);
    for (int i = 0; i < count; ++i) {
        const std::string number = std::to_string(i);
        info.properties.set("Bench:Number" + number, 0.25 * i);
        info.properties.set("Bench:Text" + number, "The value of a property of some length, number " + number);
        info.properties.set("Bench:Vector" + number, std::vector<float>(8, 1.5F));
        info.fits_keywords.push_back({.name = "KEY" + number,
                                      .value = std::to_string(i * 3),
                                      .comment = "The comment of a keyword of some length"});
    }
    const std::vector<std::byte> pixels(std::size_t{64} * 64 * 2);
    return write_unit(info, pixels, {});
}

void open_unit(benchmark::State& state, int count)
{
    const std::vector<std::byte> unit = rich_unit(count);
    // The header length is the little-endian 32-bit number after the signature (spec §9.2).
    std::uint64_t header = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        header |= std::to_integer<std::uint64_t>(unit[8 + i]) << (8 * i);
    }
    while (state.KeepRunning()) {
        const reader file(std::make_unique<memory_source>(std::span(unit)));
        benchmark::DoNotOptimize(file.images().data());
    }
    count_bytes(state, header);
}

// ---------------------------------------------------------------------------------------------------------------------
// Uncompressed reads against raw reads

void read_into(benchmark::State& state, std::unique_ptr<input_source> source)
{
    const reader file(std::move(source));
    const std::size_t size = file.image(0).data_size();
    std::vector<std::byte> destination(size);
    while (state.KeepRunning()) {
        file.read_pixels(0, destination);
        benchmark::DoNotOptimize(destination.data());
    }
    count_bytes(state, size);
}

void read_frame_in(benchmark::State& state, medium where)
{
    read_into(state, frame_unit().open(where));
}

// The pixel data of the frame are its only attached block, at the end of the unit.
void read_raw_in(benchmark::State& state, medium where)
{
    const std::unique_ptr<input_source> source = frame_unit().open(where);
    const std::size_t size = frame().size();
    std::vector<std::byte> destination(size);
    const std::uint64_t position = source->size() - size;
    while (state.KeepRunning()) {
        source->read(position, destination);
        benchmark::DoNotOptimize(destination.data());
    }
    count_bytes(state, size);
}

// ---------------------------------------------------------------------------------------------------------------------
// Storage conversion

void convert_samples(benchmark::State& state, samples format, pixel_storage from, std::size_t side, int threads)
{
    const std::vector<std::byte> source = sky_image(side, side, 3, format);
    std::vector<std::byte> destination(source.size());
    const std::size_t sample_size = format == samples::float32 ? 4 : 2;
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            detail::convert_storage(source, destination, from, side * side, 3, sample_size);
            benchmark::DoNotOptimize(destination.data());
        }
    });
    count_bytes(state, source.size());
}

// ---------------------------------------------------------------------------------------------------------------------
// Checksummed reads of the small sky

void read_checked(benchmark::State& state, const encoding& how)
{
    const std::vector<std::byte>& pixels = small_sky(samples::float32);
    const std::vector<std::byte> unit = write_unit(gray_image(1024, 1024, sample_format::float32), pixels, how);
    read_into(state, std::make_unique<memory_source>(std::span(unit)));
}

// ---------------------------------------------------------------------------------------------------------------------
// The frame in subblocks

void write_frame(benchmark::State& state, const encoding& how, int threads)
{
    const image_info info = gray_image(frame_side, frame_side, sample_format::float32);
    writer output(options_of(how));
    output.add_image(info, frame());
    std::uint64_t written = 0;
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            discarding_sink sink;
            output.save(sink);
            written = sink.position();
        }
    });
    count_bytes(state, frame().size());
    state.counters["ratio"] = static_cast<double>(frame().size()) / static_cast<double>(written);
}

void read_frame(benchmark::State& state, const encoding& how, int threads)
{
    const std::vector<std::byte> unit =
        write_unit(gray_image(frame_side, frame_side, sample_format::float32), frame(), how);
    const reader file(std::make_unique<memory_source>(std::span(unit)));
    std::vector<std::byte> destination(frame().size());
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            file.read_pixels(0, destination);
            benchmark::DoNotOptimize(destination.data());
        }
    });
    count_bytes(state, frame().size());
}

// The decompression alone, from stored bytes in memory into memory of the caller.
void decompress_frame(benchmark::State& state, std::uint64_t subblock_size, int threads)
{
    const detail::compressed_block block = detail::compress_block(
        frame(), {.codec = detail::compression_codec::zstd, .item_size = 4, .max_subblock_size = subblock_size});
    std::vector<std::byte> destination(frame().size());
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            detail::decompress_block_into(block.data, block.compression, {}, destination);
            benchmark::DoNotOptimize(destination.data());
        }
    });
    count_bytes(state, frame().size());
}

// Unshuffling the frame as a whole, in 4-byte items.
void unshuffle_frame(benchmark::State& state, int threads)
{
    std::vector<std::byte> destination(frame().size());
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            detail::unshuffle_bytes(frame(), destination, 4);
            benchmark::DoNotOptimize(destination.data());
        }
    });
    count_bytes(state, frame().size());
}

std::string subblock_label(std::uint64_t size)
{
    return size == 0 ? "whole" : "sub" + std::to_string(size / mib) + "M";
}

// ---------------------------------------------------------------------------------------------------------------------
// Regions of interest

// The tiles are 512 pixels high, and as wide unless a stacking says otherwise.
constexpr std::uint64_t tile_side = 512;

// The frames of a stacking.
constexpr std::size_t stack_size = 32;

// The tile of width x tile_side pixels at the centre of an image of image_width x image_height pixels, every channel.
pixel_region centre_tile(std::uint64_t image_width, std::uint64_t image_height, std::uint64_t width)
{
    pixel_region tile;
    tile.origin = {(image_width - width) / 2, (image_height - tile_side) / 2};
    tile.size = {width, tile_side};
    return tile;
}

// Reads region of image 0 of the unit that source holds into memory of the caller, in a task arena of threads.
void read_region(benchmark::State& state, std::unique_ptr<input_source> source, const pixel_region& region, int threads)
{
    const reader file(std::move(source));
    pixel_read_options options;
    options.region = region;
    const image_info& info = file.image(0);
    const std::uint64_t size = region.sample_count(info.geometry) * sample_size(info.sample_format);
    std::vector<std::byte> destination(size);
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            file.read_pixels(0, destination, options);
            benchmark::DoNotOptimize(destination.data());
        }
    });
    count_bytes(state, size);
}

// The small sky, uncompressed: a tile of it is the region read that the smoke run of the benchmarks covers.
const stored_unit& sky_unit()
{
    static const stored_unit unit(
        "openxisf-benchmark-sky.xisf",
        write_unit(gray_image(1024, 1024, sample_format::float32), small_sky(samples::float32), {}));
    return unit;
}

void read_sky_region(benchmark::State& state, medium where)
{
    read_region(state, sky_unit().open(where), centre_tile(1024, 1024, tile_side), 1);
}

void read_frame_region(benchmark::State& state, medium where)
{
    read_region(state, frame_unit().open(where), centre_tile(frame_side, frame_side, tile_side), 1);
}

// The least that a read of the tile of the frame costs: one read for each of its rows, straight into place, from the
// pixel data at the end of the unit.
void read_tile_rows(benchmark::State& state, medium where)
{
    const std::unique_ptr<input_source> source = frame_unit().open(where);
    const pixel_region tile = centre_tile(frame_side, frame_side, tile_side);
    const std::uint64_t row = tile_side * 4;
    const std::uint64_t start = source->size() - frame().size();
    std::vector<std::byte> destination(tile_side * row);
    while (state.KeepRunning()) {
        for (std::uint64_t y = 0; y < tile_side; ++y) {
            const std::uint64_t pixel = ((tile.origin[1] + y) * frame_side) + tile.origin[0];
            source->read(start + (pixel * 4), std::span(destination).subspan(y * row, row));
        }
        benchmark::DoNotOptimize(destination.data());
    }
    count_bytes(state, destination.size());
}

// The tile of the frame compressed as how says, read from memory.
void read_compressed_region(benchmark::State& state, const encoding& how, int threads)
{
    read_region(state,
                std::make_unique<memory_source>(
                    write_unit(gray_image(frame_side, frame_side, sample_format::float32), frame(), how)),
                centre_tile(frame_side, frame_side, tile_side), threads);
}

// The colour image of the conversions in normal storage, uncompressed. Its samples are those of the planar sky, which
// a read moves as they are.
const stored_unit& rgb_unit()
{
    static const stored_unit unit("openxisf-benchmark-rgb.xisf", [] {
        image_info info = gray_image(rgb_side, rgb_side, sample_format::float32);
        info.geometry.channels = 3;
        info.color_space = color_space::rgb;
        info.pixel_storage = pixel_storage::normal;
        return write_unit(info, sky_image(rgb_side, rgb_side, 3, samples::float32), {});
    }());
    return unit;
}

void read_rgb_in(benchmark::State& state, medium where)
{
    read_into(state, rgb_unit().open(where));
}

// One channel of the tile of the colour image: a run of one sample for each pixel.
void read_channel_region(benchmark::State& state, medium where)
{
    pixel_region tile = centre_tile(rgb_side, rgb_side, tile_side);
    tile.first_channel = 1;
    tile.channel_count = 1;
    read_region(state, rgb_unit().open(where), tile, 1);
}

// A stacking: frames of width x height Float32 samples, of which the tile of tile_width x tile_side pixels at the
// centre is read.
struct stack_shape
{
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t tile_width = 0;

    [[nodiscard]] std::string name() const
    {
        return std::to_string(width) + "x" + std::to_string(height) + "/tile" + std::to_string(tile_width);
    }
};

// The frames of a stacking of frames of width x height samples written as how says: copies of one unit, each in memory
// of its own and in a file of its own. Made on first use, and kept.
const std::vector<std::unique_ptr<stored_unit>>& stack_of(std::uint64_t width, std::uint64_t height,
                                                          const encoding& how)
{
    static std::map<std::string, std::vector<std::unique_ptr<stored_unit>>> stacks;
    const std::string key = std::to_string(width) + "x" + std::to_string(height) + "/" + how.name;
    auto found = stacks.find(key);
    if (found == stacks.end()) {
        const std::vector<std::byte> unit = write_unit(gray_image(width, height, sample_format::float32),
                                                       sky_image(width, height, 1, samples::float32), how);
        std::vector<std::unique_ptr<stored_unit>> frames;
        frames.reserve(stack_size);
        for (std::size_t k = 0; k < stack_size; ++k) {
            frames.push_back(std::make_unique<stored_unit>(
                "openxisf-benchmark-stack-" + std::to_string(stacks.size()) + "-" + std::to_string(k) + ".xisf", unit));
        }
        found = stacks.emplace(key, std::move(frames)).first;
    }
    return found->second;
}

// The stacking pattern: the same tile of each frame, read as a region of each, as a stacking that combines the frames
// tile by tile reads them.
void read_stack(benchmark::State& state, const stack_shape& shape, const encoding& how, medium where, int threads)
{
    const std::vector<std::unique_ptr<stored_unit>>& frames = stack_of(shape.width, shape.height, how);
    std::vector<reader> inputs;
    inputs.reserve(frames.size());
    for (const std::unique_ptr<stored_unit>& frame : frames) {
        inputs.emplace_back(frame->open(where));
    }
    pixel_read_options options;
    options.region = centre_tile(shape.width, shape.height, shape.tile_width);
    const std::uint64_t size = shape.tile_width * tile_side * 4;
    std::vector<std::byte> tile(size);
    std::vector<std::byte> expected(size);
    reader(frames.front()->open(medium::memory)).read_pixels(0, expected, options);
    inputs.front().read_pixels(0, tile, options);
    if (tile != expected) {
        state.SkipWithError("the tile differs from the one read from memory");
        return;
    }
    in_arena(threads, [&] {
        while (state.KeepRunning()) {
            for (const reader& input : inputs) {
                input.read_pixels(0, tile, options);
                benchmark::DoNotOptimize(tile.data());
            }
        }
    });
    count_bytes(state, size * frames.size());
}

// An editor that fills a reserved frame tile by tile from threads threads, each taking the next tile in turn, as a
// stacking writes its output. Nothing is committed: without a checksum, commit() would only flush the file.
void fill_frame(benchmark::State& state, medium where, std::size_t threads)
{
    static const std::vector<std::byte> reserved = [] {
        writer output(options_of({}));
        (void)output.add_reserved_image(gray_image(frame_side, frame_side, sample_format::float32));
        memory_sink sink;
        output.save(sink);
        return sink.release();
    }();
    std::vector<std::byte> bytes = reserved;
    std::optional<temporary_file> file;
    std::unique_ptr<random_access_device> device;
    if (where == medium::memory) {
        device = std::make_unique<memory_device>(std::span(bytes));
    } else {
        file.emplace("openxisf-benchmark-reserved.xisf", reserved);
        device = std::make_unique<file_device>(file->path());
    }
    editor output(std::move(device));
    std::vector<pixel_write_options> tiles;
    for (std::uint64_t y = 0; y < frame_side; y += tile_side) {
        for (std::uint64_t x = 0; x < frame_side; x += tile_side) {
            pixel_region region;
            region.origin = {x, y};
            region.size = {tile_side, tile_side};
            pixel_write_options options;
            options.region = std::move(region);
            tiles.push_back(std::move(options));
        }
    }
    const std::span<const std::byte> data = std::span(frame()).first(tile_side * tile_side * 4);
    while (state.KeepRunning()) {
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                for (std::size_t k = next++; k < tiles.size(); k = next++) {
                    output.write_pixels(0, data, tiles[k]);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    count_bytes(state, frame().size());
}

} // namespace

void register_unit_benchmarks()
{
    // Opening is linear in the size of the header: ten times the elements take about ten times as long.
    for (const int count : {500, 5000}) {
        benchmark::RegisterBenchmark("open/rich-header/" + std::to_string(count), open_unit, count)
            ->Unit(benchmark::kMicrosecond);
    }

    // Reads from files count wall time, of which waiting for the disk is part.
    for (const auto& [where, name] : media) {
        benchmark::RegisterBenchmark("frame/read/" + std::string(name), read_frame_in, where)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
        benchmark::RegisterBenchmark("frame/raw/" + std::string(name), read_raw_in, where)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
    }

    for (const int threads : arena_sizes()) {
        const std::string arena = "/arena" + std::to_string(threads);
        for (const auto& [format, name] : {std::pair{samples::uint16, "u16"}, std::pair{samples::float32, "f32"}}) {
            benchmark::RegisterBenchmark("convert/planar-to-normal/" + std::string(name) + "/1024" + arena,
                                         convert_samples, format, pixel_storage::planar, std::size_t{1024}, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
            benchmark::RegisterBenchmark("convert/normal-to-planar/" + std::string(name) + "/1024" + arena,
                                         convert_samples, format, pixel_storage::normal, std::size_t{1024}, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
        benchmark::RegisterBenchmark("frame/convert/planar-to-normal/f32/" + std::to_string(rgb_side) + arena,
                                     convert_samples, samples::float32, pixel_storage::planar, rgb_side, threads)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
    }

    for (const auto& [compression, compression_name] :
         {std::pair{std::optional<openxisf::codec>{}, "none"}, std::pair{std::optional{codec::zstd}, "zstd+sh"}}) {
        for (const auto& [checksum, checksum_name] :
             {std::pair{std::optional<checksum_algorithm>{}, "none"},
              std::pair{std::optional{checksum_algorithm::sha1}, "sha1"},
              std::pair{std::optional{checksum_algorithm::sha256}, "sha256"},
              std::pair{std::optional{checksum_algorithm::sha512}, "sha512"},
              std::pair{std::optional{checksum_algorithm::sha3_256}, "sha3-256"},
              std::pair{std::optional{checksum_algorithm::sha3_512}, "sha3-512"}}) {
            const encoding how{.name = std::string(compression_name) + "/" + checksum_name,
                               .codec = compression,
                               .checksum = checksum};
            benchmark::RegisterBenchmark("read/sky/" + how.name, read_checked, how)->Unit(benchmark::kMillisecond);
        }
    }

    const std::vector<encoding> frame_encodings{
        {.name = "zstd+sh/" + subblock_label(0), .codec = codec::zstd},
        {.name = "zstd+sh/" + subblock_label(mib), .codec = codec::zstd, .subblock_size = mib},
        {.name = "zstd+sh/" + subblock_label(4 * mib), .codec = codec::zstd, .subblock_size = 4 * mib},
        {.name = "zstd+sh/" + subblock_label(16 * mib), .codec = codec::zstd, .subblock_size = 16 * mib},
        {.name = "lz4+sh/" + subblock_label(0), .codec = codec::lz4},
        {.name = "lz4+sh/" + subblock_label(4 * mib), .codec = codec::lz4, .subblock_size = 4 * mib},
        {.name = "zlib+sh/" + subblock_label(0), .codec = codec::zlib},
        {.name = "zlib+sh/" + subblock_label(4 * mib), .codec = codec::zlib, .subblock_size = 4 * mib},
    };
    for (const int threads : arena_sizes()) {
        const std::string arena = "/arena" + std::to_string(threads);
        for (const encoding& how : frame_encodings) {
            benchmark::RegisterBenchmark("frame/write/" + how.name + arena, write_frame, how, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
            benchmark::RegisterBenchmark("frame/read/" + how.name + arena, read_frame, how, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
        // The decompression alone, and the unshuffling that follows it, which reads and writes the whole block.
        for (const std::uint64_t size : {std::uint64_t{0}, mib, 4 * mib}) {
            benchmark::RegisterBenchmark("frame/decompress/zstd+sh/" + subblock_label(size) + arena, decompress_frame,
                                         size, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
        benchmark::RegisterBenchmark("frame/unshuffle/4" + arena, unshuffle_frame, threads)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
    }

    // Regions of interest. The tile of the frame against the whole read (frame/read) and against reading its rows
    // alone; one channel of a tile of the colour image in normal storage against its whole read.
    for (const auto& [where, name] : media) {
        benchmark::RegisterBenchmark("region/sky/" + std::string(name), read_sky_region, where)
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
        benchmark::RegisterBenchmark("frame/region/none/" + std::string(name), read_frame_region, where)
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
        benchmark::RegisterBenchmark("frame/region-rows/none/" + std::string(name), read_tile_rows, where)
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
        benchmark::RegisterBenchmark("frame/region/rgb-normal/channel/" + std::string(name), read_channel_region, where)
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
        benchmark::RegisterBenchmark("frame/read/rgb-normal/" + std::string(name), read_rgb_in, where)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
    }
    // The tile of the frame in subblocks of 4 MiB, with and without shuffling, against the whole read.
    const encoding zstd_sub4{.name = "zstd/" + subblock_label(4 * mib),
                             .codec = codec::zstd,
                             .subblock_size = 4 * mib,
                             .byte_shuffle = false};
    const encoding zstd_shuffled_sub4{
        .name = "zstd+sh/" + subblock_label(4 * mib), .codec = codec::zstd, .subblock_size = 4 * mib};
    for (const int threads : arena_sizes()) {
        const std::string arena = "/arena" + std::to_string(threads);
        benchmark::RegisterBenchmark("frame/read/" + zstd_sub4.name + arena, read_frame, zstd_sub4, threads)
            ->Unit(benchmark::kMillisecond)
            ->UseRealTime();
        for (const encoding& how : {zstd_sub4, zstd_shuffled_sub4}) {
            benchmark::RegisterBenchmark("frame/region/" + how.name + arena, read_compressed_region, how, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
    }
    // The stacking pattern over 32 frames: uncompressed, their rows apart by 18, 70, 142 and 254 KiB, which the reads
    // coalesce or not, and a wide tile that holds a quarter of the bytes between its first and its last; then frames
    // meant for region access, compressed in subblocks of 4 MiB with shuffling.
    const std::array shapes{stack_shape{.width = 5120, .height = 5120, .tile_width = 512},
                            stack_shape{.width = 18432, .height = 1024, .tile_width = 512},
                            stack_shape{.width = 36864, .height = 768, .tile_width = 512},
                            stack_shape{.width = 65536, .height = 512, .tile_width = 512},
                            stack_shape{.width = 65536, .height = 512, .tile_width = 16384}};
    for (const stack_shape& shape : shapes) {
        for (const auto& [where, name] : media) {
            benchmark::RegisterBenchmark("frame/stack/none/" + shape.name() + "/" + name, read_stack, shape,
                                         encoding{.name = "none"}, where, 1)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
    }
    for (const int threads : arena_sizes()) {
        for (const auto& [where, name] : media) {
            benchmark::RegisterBenchmark("frame/stack/" + zstd_shuffled_sub4.name + "/" + shapes.front().name() + "/" +
                                             name + "/arena" + std::to_string(threads),
                                         read_stack, shapes.front(), zstd_shuffled_sub4, where, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
    }
    // An editor filling a reserved frame of 100 tiles, in memory and in a file, from several threads.
    for (const auto& [where, name] : {media[0], media[1]}) {
        for (const std::size_t threads : {std::size_t{1}, std::size_t{8}, std::size_t{16}}) {
            benchmark::RegisterBenchmark("frame/fill/" + std::string(name) + "/threads" + std::to_string(threads),
                                         fill_frame, where, threads)
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime();
        }
    }
}

} // namespace openxisf::bench
