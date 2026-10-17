// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Zstandard (spec §10.6.9): exactly one frame of the Zstandard format (RFC 8878) for each subblock. Only the stable API
// of the library is used, so that any build of it links.

#include <openxisf/error.h>

#include "codec/codecs.h"

#include <zstd.h>
// The values of ZSTD_ErrorCode, which ZSTD_getErrorCode() returns. That function is declared in zstd.h from 1.5.7 on,
// and here before, and every build exports it. zstd_errors.h calls its API static-only, but these are constants, and
// the two used here are below 100, the values that it declares stable.
#include <zstd_errors.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <new>
#include <string>

namespace openxisf::detail {

static_assert(zstd_default_level == ZSTD_CLEVEL_DEFAULT);

namespace {

struct context_deleter
{
    void operator()(ZSTD_DCtx* context) const noexcept
    {
        (void)ZSTD_freeDCtx(context);
    }
    void operator()(ZSTD_CCtx* context) const noexcept
    {
        (void)ZSTD_freeCCtx(context);
    }
};

[[noreturn]] void throw_corrupt(const std::string& message)
{
    throw integrity_error(errc::corrupt_compressed_data, message);
}

[[noreturn]] void throw_failure(std::size_t result)
{
    if (ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation) {
        throw std::bad_alloc();
    }
    throw unsupported_error(errc::codec_failure, "Zstandard failed: " + std::string(ZSTD_getErrorName(result)));
}

// Checks the result of a call that only fails for reasons other than the data.
std::size_t checked(std::size_t result)
{
    if (ZSTD_isError(result) != 0) {
        throw_failure(result);
    }
    return result;
}

// The window log for a window of at most max_window bytes; the library allows a window of 2^log bytes.
int window_log(std::uint64_t max_window)
{
    const ZSTD_bounds bounds = ZSTD_dParam_getBounds(ZSTD_d_windowLogMax);
    if (ZSTD_isError(bounds.error) != 0) {
        throw_failure(bounds.error);
    }
    if (max_window == 0) {
        return bounds.upperBound;
    }
    // The position of the highest bit that is set: the base-2 logarithm, rounded down.
    return std::clamp(63 - std::countl_zero(max_window), bounds.lowerBound, bounds.upperBound);
}

} // namespace

// The input must be exactly one frame of compressed data: not a skippable frame, nor a frame of an older format, and
// with nothing after it.
void zstd_check_frame(std::span<const std::byte> input, std::uint64_t expected)
{
    const auto byte_at = [&input](std::size_t i) { return static_cast<std::uint32_t>(input[i]) << (8 * i); };
    if (input.size() < 4 || (byte_at(0) | byte_at(1) | byte_at(2) | byte_at(3)) != ZSTD_MAGICNUMBER) {
        throw_corrupt("the Zstandard data do not start with a Zstandard frame");
    }
    const std::size_t frame = ZSTD_findFrameCompressedSize(input.data(), input.size());
    if (ZSTD_isError(frame) != 0) {
        throw_corrupt("the Zstandard frame is corrupt: " + std::string(ZSTD_getErrorName(frame)));
    }
    if (frame != input.size()) {
        throw_corrupt("the Zstandard frame of " + std::to_string(frame) + " bytes is followed by " +
                      std::to_string(input.size() - frame) + " more bytes");
    }
    const unsigned long long declared = ZSTD_getFrameContentSize(input.data(), input.size());
    if (declared == ZSTD_CONTENTSIZE_ERROR) {
        throw_corrupt("the header of the Zstandard frame is corrupt");
    }
    if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != expected) {
        throw_corrupt("the Zstandard frame declares " + std::to_string(declared) + " bytes, not " +
                      std::to_string(expected));
    }
}

void zstd_decompress(std::span<const std::byte> input, std::span<std::byte> output, std::uint64_t max_window)
{
    zstd_check_frame(input, output.size());
    const std::unique_ptr<ZSTD_DCtx, context_deleter> context(ZSTD_createDCtx());
    if (!context) {
        throw std::bad_alloc();
    }
    (void)checked(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, window_log(max_window)));

    // An empty output gets a pointer to this byte rather than a null pointer; nothing is written to it.
    std::byte placeholder{};
    ZSTD_inBuffer in{.src = input.data(), .size = input.size(), .pos = 0};
    ZSTD_outBuffer out{.dst = output.empty() ? &placeholder : output.data(), .size = output.size(), .pos = 0};
    for (;;) {
        const std::size_t consumed = in.pos;
        const std::size_t produced = out.pos;
        const std::size_t result = ZSTD_decompressStream(context.get(), &out, &in);
        if (ZSTD_isError(result) != 0) {
            switch (ZSTD_getErrorCode(result)) {
            case ZSTD_error_frameParameter_windowTooLarge:
                throw limit_error(errc::zstd_window_too_large,
                                  "the Zstandard frame needs a window larger than the limit of " +
                                      std::to_string(std::uint64_t{1} << window_log(max_window)) + " bytes");
            case ZSTD_error_memory_allocation:
                throw std::bad_alloc();
            default:
                throw_corrupt("the Zstandard data are corrupt: " + std::string(ZSTD_getErrorName(result)));
            }
        }
        // 0 means that the frame is decoded and flushed, and its checksum, if it has one, verified.
        if (result == 0) {
            break;
        }
        if (in.pos == consumed && out.pos == produced) {
            throw_corrupt(out.pos == out.size
                              ? "the Zstandard data decode to more than " + std::to_string(output.size()) + " bytes"
                              : "the Zstandard data end before the end of the frame");
        }
    }
    if (out.pos != output.size()) {
        throw_corrupt("the Zstandard data decode to " + std::to_string(out.pos) + " bytes, not " +
                      std::to_string(output.size()));
    }
}

bool zstd_needs_window(std::span<const std::byte> input) noexcept
{
    return ZSTD_getFrameContentSize(input.data(), input.size()) == ZSTD_CONTENTSIZE_UNKNOWN;
}

struct zstd_encoder::state
{
    std::unique_ptr<ZSTD_CCtx, context_deleter> context;
};

zstd_encoder::zstd_encoder(int level) : state_(std::make_unique<state>())
{
    state_->context.reset(ZSTD_createCCtx());
    if (!state_->context) {
        throw std::bad_alloc();
    }
    (void)checked(ZSTD_CCtx_setParameter(state_->context.get(), ZSTD_c_compressionLevel, level));
}

zstd_encoder::~zstd_encoder() = default;

// ZSTD_compress2() starts a new frame with the parameters of the context, whatever the context did before.
void zstd_encoder::compress(std::span<const std::byte> input, std::vector<std::byte>& output)
{
    const std::size_t start = output.size();
    output.resize(start + ZSTD_compressBound(input.size()));
    const std::size_t written = checked(ZSTD_compress2(state_->context.get(), output.data() + start,
                                                       output.size() - start, input.data(), input.size()));
    output.resize(start + written);
}

void zstd_compress(std::span<const std::byte> input, int level, std::vector<std::byte>& output)
{
    zstd_encoder(level).compress(input, output);
}

} // namespace openxisf::detail
