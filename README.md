# OpenXISF

OpenXISF is a C++20 library for reading and writing XISF, the Extensible Image Serialization Format used by
PixInsight. It is written from the published specification and aims at full conformance with
[XISF 1.0, Revision 1](https://pixinsight.com/doc/docs/XISF-1.0-spec/XISF-1.0-spec.html), with an API that does not
depend on any application.

**Status: 1.0.1.** It reads and writes monolithic and distributed units, implements the algorithms that the
specification defines, evaluates astrometric solutions, and reads signed units, whose signatures it returns without
verifying them. The API is stable: from 1.0 on, only a new major version may break it. The ABI holds within a minor
version: the shared library of each minor version has a soname of its own (`libopenxisf.so.1.0` for 1.0.x), so a
program built against 1.0 is rebuilt to use 1.1. The Windows DLL has one name in every version (`openxisf.dll`,
`libopenxisf.dll` with MinGW), so nothing keeps a program built against 1.0 from loading the DLL of 1.1 that it
finds first: ship the DLL with the program.

- **Reading.** `openxisf::reader` checks the header of a unit (the file structure, the XML, the root element, the
  elements of the specification and the references between them) and its data blocks (where each one is, how it is
  encoded and compressed, and its checksum, which is verified with SHA-1, SHA-256, SHA-512, SHA3-256 or SHA3-512 before
  the block is used). It reads the properties of the unit, of its images and the standalone ones, with values of every
  type of the specification, and tables.
- **Images.** Their geometry, sample format, colour space and other attributes; their pixel data, decompressed and in
  native byte order, in either storage model, into memory of the library or of the application, with progress and
  cancellation; and what describes each image: FITS keywords, ICC profile, RGB working space, display function, colour
  filter array, resolution and thumbnail.
- **Distributed units.** A header file whose data blocks are in data blocks files (`.xisb`) or other files is read the
  same way: a resolver opens each file, by default only inside the directory of the header file, and the block index of
  each data blocks file is checked.
- **Tolerance.** A problem confined to one object makes that object unavailable, with a diagnostic, and the rest of the
  unit is read. Departures from the specification that are unambiguous and harmless are read with a warning, such as
  uppercase hexadecimal digits, codec and checksum algorithm names in another case, FITS keyword names padded with
  spaces, which are read without them, or control characters that XML 1.0 does not allow; what affects safety or the
  meaning of the data is refused. A compressed block or subblock whose compressed and uncompressed sizes are equal is
  read as stored, as PixInsight writes and reads such subblocks.
- **Writing.** `openxisf::writer` writes the same model back: it checks a whole unit against the specification before
  it writes a byte of it (an astrometric solution only for the types of its properties), and writes its metadata,
  images, properties and tables, with every block uncompressed and without checksums unless asked otherwise, so that
  every XISF 1.0 decoder reads the result. On request it compresses blocks with zlib, LZ4, LZ4HC or Zstandard, with
  byte shuffling and subblocks, adds checksums and generates UUIDs. It writes a monolithic file, or a distributed unit:
  a header file and a data blocks file next to it. Files are replaced only once they are complete. What several images
  share is written for each of them, without `uid` attributes or `Reference` elements.
- **Signed units.** A unit whose header has an XML signature after its root element is read like any other, and its
  signature is not verified: the reader returns it and the root element that it covers, exactly as written, for the XML
  signature tooling of the application. A signature that is malformed or does not name the root element does not
  prevent reading the unit; it is a warning, as is a block stored outside the header of a signed unit without the
  checksum through which the signature covers it. The writer never signs: a signed unit written again is unsigned.
- **Algorithms.** The algorithms of the specification are there to show and convert images: the colour transformations
  of Annex B between RGB, CIE XYZ and CIE L*a*b*, relative to the RGB working space of an image, with helpers that
  convert pixel data to CIE L*a*b* before writing and back to RGB after reading; display functions, and the adaptive
  algorithm that computes one from the statistics of an image; the orientation of an image, applied for showing it
  only; and property values written as text with their format specifiers.
- **Astrometric solutions.** The properties of the `AstrometricSolution` namespace of an image are read with each of
  their layers checked: the projection, with the seven projection systems of the specification and its linear
  transformation, the projective transformations, and the distortion models of radial basis function splines, Global,
  Local and Fallback terms. A solution converts image coordinates to celestial coordinates and back through its highest
  available layer, in double precision and with the inverse that each layer stores. The writer keeps the properties of
  a solution as they are given.

The repository also has the build, test and packaging infrastructure, the error types and safety limits of the API, the
input and output layer, and the internal building blocks: the text forms of numbers and Boolean values, Base64 and
hexadecimal data, UUIDs and time stamps.

## Features and conformance

| Area | Reading | Writing |
|---|---|---|
| Monolithic units (`.xisf`) | Yes | Yes |
| Distributed units (`.xish` headers and `.xisb` blocks files) | Yes | Yes |
| Data blocks: attachment, embedded, inline | Yes | Attachment and inline |
| Data blocks: `url()` and `path()` | Yes, through a resolver: by default `path()` inside the header's directory | `path()` in the header's directory |
| Checksums: SHA-1, SHA-256, SHA-512, SHA3-256, SHA3-512 | Yes | Yes |
| Compression: zlib, LZ4, LZ4HC, Zstandard, with byte shuffling and subblocks | Yes | Yes |
| Properties: scalars, complex numbers, strings, time points, vectors, matrices, tables | Yes | Yes |
| Images: every sample format, planar and normal storage, both byte orders | Yes | Yes, in little-endian byte order |
| Ancillary elements: FITS keywords, ICC profile, RGB working space, display function, color filter array, resolution, thumbnail | Yes | Yes |
| Colour transformations (Annex B), display functions and their adaptive algorithm, orientation, property format rendering | Yes | Yes |
| Astrometric solutions: every projection system and layer, evaluated in both directions | Yes | Kept as given |
| Signed units (the signature is returned, not verified) | Yes | No: the writer never signs |

The conformance matrix below lists each requirement of the specification, whether OpenXISF reads and writes it,
and the suites of `tests/` that cover it. Every row has passing tests.

| Specification | Requirement | Reading | Writing | Tests |
|---|---|---|---|---|
| §7 | Unsupported object unavailable, rest of the unit accessible; unknown elements, attributes and properties ignored | ✓ | – | `conformance/availability`, `conformance/image`, `conformance/property_element` |
| §7.1 | Baseline encoder abilities | – | ✓ | `conformance/baseline_encoder` |
| §7.2 | Baseline decoder abilities | ✓ | – | `conformance/baseline_decoder` |
| §8.1 | 8–64-bit scalars required; 128-bit optional (as data) | ✓ | ✓ | `unit/property_value`, `conformance/writer` |
| §8.2 | Little-endian structural integers | ✓ | ✓ | `unit/container`, `conformance/writer` |
| §8.3 | Decimal, radix, float and Boolean text forms; white space | ✓ | ✓ | `unit/text_grammar`, `conformance/writer` |
| §8.4.1 | Property identifier grammar and uniqueness per object | ✓ | ✓ | `conformance/property_element`, `conformance/writer_validation` |
| §8.4.3 | Format specifier grammar and rendering | ✓ | ✓ | `unit/format_specifier`, `unit/format`, `conformance/writer`, `conformance/writer_validation` |
| §8.4.4 | All property types and alternate names; complex; String UTF-8 rules; TimePoint; vectors; matrices | ✓ | ✓ | `unit/property_value`, `conformance/property_element`, `conformance/writer`, `conformance/writer_validation` |
| §8.4.4.7 | Table structure and data rules | ✓ | ✓ | `conformance/table`, `conformance/writer`, `conformance/writer_validation` |
| §8.5.1–3 | N-D images, channel rules, planar and normal storage | ✓ | ✓ | `conformance/image`, `unit/pixel_layout`, `conformance/writer`, `conformance/writer_validation` |
| §8.5.4 | Colour spaces; RGB working space and derived luminance | ✓ | ✓ | `conformance/rgbws`, `unit/color`, `conformance/writer`, `conformance/writer_validation` |
| §8.5.5 | Representable range defaults and bounds | ✓ | ✓ | `conformance/image`, `conformance/writer`, `conformance/writer_validation` |
| §8.5.6–7 | Display function evaluation and adaptive algorithm | ✓ | ✓ | `unit/display`, `conformance/writer_validation` |
| §9.1–9.2 | Monolithic file layout, unused space zero | ✓ | ✓ | `unit/container`, `conformance/writer` |
| §9.3–9.4 | Header files; data blocks files and block index | ✓ | ✓ | `unit/blocks_file`, `conformance/distributed` |
| §9.5 | XML declaration, initial comment, root element, namespaces, extension elements | ✓ | ✓ | `conformance/header`, `conformance/writer` |
| §9.5 | Detached XML signature handling | ✓ | – | `conformance/signature` |
| §9.6 | File name suffixes | – | ✓ | `conformance/writer`, `conformance/distributed` |
| §10.1–10.3 | Block location forms, parenthesis rule, attachment bounds | ✓ | ✓ | `unit/block_attributes`, `unit/data_block`, `conformance/writer`, `conformance/distributed` |
| §10.4 | Byte order attribute | ✓ | ✓ | `conformance/byte_order`, `conformance/writer` |
| §10.5 | Checksum algorithms, digest rules, verification before use | ✓ | ✓ | `unit/hash`, `conformance/checksum`, `conformance/writer` |
| §10.6 | Compression attribute, unsupported codec, subblocks | ✓ | ✓ | `unit/codecs`, `unit/compressed_block`, `conformance/writer` |
| §10.6.1 | Digest over compressed data; no decompression after a failed check | ✓ | ✓ | `conformance/checksum`, `conformance/compression`, `conformance/writer` |
| §10.6.2 | Byte shuffling including the tail bytes | ✓ | ✓ | `unit/shuffle`, `conformance/writer` |
| §10.6.3–10 | zlib, LZ4, LZ4HC and Zstandard formats, with and without shuffling | ✓ | ✓ | `unit/codecs`, `samples`, `conformance/writer` |
| §11 | `uid` grammar and uniqueness; core element names case-sensitive | ✓ | – | `conformance/header` |
| §11.1 | Property element placement and serialization by type | ✓ | ✓ | `conformance/property_element`, `conformance/writer`, `conformance/writer_validation` |
| §11.2–11.3 | Structure, Field, Table, Row, Cell | ✓ | ✓ | `conformance/table`, `conformance/writer`, `conformance/writer_validation` |
| §11.4 | Metadata element, mandatory and optional properties, reserved namespace | ✓ | ✓ | `conformance/metadata`, `conformance/writer`, `conformance/writer_validation` |
| §11.5.1–2 | Image attributes | ✓ | ✓ | `conformance/image`, `conformance/writer`, `conformance/writer_validation` |
| §11.5.2 | Orientation applied for visual use only (helper) | ✓ | – | `unit/orientation`, `conformance/writer` |
| §11.5.3 | Reserved astronomical property identifiers and types | ✓ | ✓ | `unit/property_catalog`, `conformance/writer_validation` |
| §11.5.3.7 | Astrometric solutions: layers, availability, versioning, evaluation, preservation | ✓ | ✓ | `unit/astrometry`, `samples`, `conformance/writer` |
| §11.6 | FITSKeyword | ✓ | ✓ | `conformance/fits_keyword`, `conformance/writer`, `conformance/writer_validation` |
| §11.7 | ICCProfile (unaltered, embedded flag set on write, no byteOrder) | ✓ | ✓ | `conformance/icc`, `conformance/writer`, `conformance/writer_validation` |
| §11.8 | RGBWorkingSpace | ✓ | ✓ | `conformance/rgbws`, `conformance/writer`, `conformance/writer_validation` |
| §11.9 | DisplayFunction element | ✓ | ✓ | `conformance/display_function`, `conformance/writer`, `conformance/writer_validation` |
| §11.10 | ColorFilterArray | ✓ | ✓ | `conformance/cfa`, `conformance/writer`, `conformance/writer_validation` |
| §11.11 | Resolution | ✓ | ✓ | `conformance/resolution`, `conformance/writer`, `conformance/writer_validation` |
| §11.12 | Thumbnail restrictions | ✓ | ✓ | `conformance/thumbnail`, `conformance/writer`, `conformance/writer_validation` |
| §11.13 | Reference, no chained references, root-level image references | ✓ | – | `conformance/reference` |
| Annex B | RGB ↔ XYZ ↔ L\*a\*b\*, grayscale component | ✓ | ✓ | `unit/color` |

## Requirements and platforms

- C++20 and CMake 3.25 or newer. 64-bit targets only (x86-64 and arm64); configuring for a 32-bit toolchain stops with
  an error.
- Compilers, each tested by CI at the oldest version given: MSVC from Visual Studio 2022, and clang-cl from Visual
  Studio 2026; on Linux, GCC 11, and Clang 15 with libstdc++ or Clang 18 with libc++; the current GCC and Clang of
  MSYS2; AppleClang from Xcode 16, with a macOS deployment target of 13.3 or later.
- Platforms: Windows (MSVC, clang-cl, MSYS2 UCRT64 and CLANG64), Linux (GCC and Clang, with libstdc++ or libc++) and
  macOS.
- Dependencies: pugixml 1.11 or newer, for its `pugixml::pugixml` target, zlib, LZ4 and Zstandard; CI also builds with
  the oldest versions of Ubuntu 22.04: pugixml 1.12.1, zlib 1.2.11, LZ4 1.9.3, Zstandard 1.4.8 and GoogleTest 1.11.
  oneTBB and OpenSSL are optional. GoogleTest is needed for the tests, and Google Benchmark 1.8 or newer for the
  benchmarks.

## Building

The build is described by CMake presets: `msvc`, `clang-cl`, `msys2-ucrt64-gcc`, `msys2-clang64`, `linux-gcc`,
`linux-clang`, `linux-clang-libcxx` and `macos`, each with a `-debug` and a `-release` variant. The presets that do not
fit your host are hidden by `cmake --list-presets`.

```
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

Binaries go to `out/build/<preset>`. Personal settings belong in a `CMakeUserPresets.json`, which git ignores. The test
presets fail a test that runs for more than 15 minutes, and a fuzzing run ten minutes past its length: some tests open
headers as large as the default limits allow, in a second or so, which a cost that grows faster than the header would
make take hours.

The tests are two programs: `openxisf_tests`, and `openxisf_allocation_tests`, the few that count the memory that code
asks for, or make its allocations fail, through an `operator new` of their own, which would take from the whole suite
the checks of a sanitizer's allocator. The `package` tests install the library into an empty staging prefix, then
configure, build and run a consumer against it, in the configuration of the run with a multi-configuration generator
(`ctest -C Debug`), and check the versions that the package accepts.

Other presets run the tests under analysis tools: `linux-gcc-asan`, `linux-clang-asan` and `msvc-asan` (AddressSanitizer;
the Linux ones add UndefinedBehaviorSanitizer), `linux-clang-tsan` (ThreadSanitizer), `linux-clang-tsan-tbb`
(ThreadSanitizer with oneTBB, over dependencies that vcpkg builds with the instrumentation) and `linux-gcc-coverage`.
`linux-clang-fuzz` builds the fuzz targets with libFuzzer, and its tests fuzz each target for a minute, starting from the
seeds in `fuzz/seeds`. The other presets run those seeds once, as a regression test.

`-DOPENXISF_BUILD_BENCHMARKS=ON` builds `openxisf_benchmarks`, which measures the codecs, byte shuffling, the
checksums, the conversion between storage models, the opening of a large header, and the reading and writing of a
100 MiB frame. Run it from a release build; its test runs every benchmark but those of the frame once, only to check
that they work.

### Dependencies

- **vcpkg, manifest mode.** Set `VCPKG_ROOT` to a vcpkg checkout. The `msvc`, `clang-cl` and `linux-clang-libcxx`
  presets use it automatically, and `vcpkg.json` pins the versions with a baseline. `linux-clang-libcxx` builds the
  dependencies against libc++ too, with the triplet of `cmake/vcpkg`: it needs libc++, and `CC` and `CXX` set to
  Clang. With the other presets add `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
  Optional dependencies are features: `-DVCPKG_MANIFEST_FEATURES="tests;benchmarks;tbb;openssl"`.
- **Linux.** `sudo apt install pkg-config libpugixml-dev zlib1g-dev liblz4-dev libzstd-dev libgtest-dev`, and
  `libbenchmark-dev` for the benchmarks, `libtbb-dev` for oneTBB and `libssl-dev` for OpenSSL.
- **MSYS2.** In the UCRT64 or CLANG64 shell, install `cmake`, `ninja`, `pkgconf`, `pugixml`, `zlib`, `lz4`, `zstd` and
  `gtest` with the matching `mingw-w64-<environment>-` prefix, plus the compiler, and `tbb` and `openssl` for the
  options that use them.
- **macOS.** `brew install pkgconf pugixml lz4 zstd googletest`, and `tbb` and `openssl@3` for the options that use them,
  then configure with `-DCMAKE_PREFIX_PATH="$(brew --prefix)"` (and `$(brew --prefix openssl@3)` for OpenSSL).

The Visual Studio presets use Ninja, which needs the compiler environment: run them from a Developer PowerShell
(or any shell where `cl` works). The MSYS2 presets run from the matching MSYS2 shell.

### Options

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared library instead of a static one |
| `OPENXISF_BUILD_TESTS` | `ON` at the top level | Build the test suites |
| `OPENXISF_BUILD_SAMPLES` | `ON` at the top level | Build the sample programs of `samples/`, which run as tests |
| `OPENXISF_BUILD_BENCHMARKS` | `OFF` | Build the benchmarks of `benchmarks/` with Google Benchmark |
| `OPENXISF_BUILD_FUZZERS` | `OFF` | Build the fuzz targets: libFuzzer programs when `OPENXISF_SANITIZE` contains `fuzzer`, seed replays otherwise (the presets turn it on) |
| `OPENXISF_FUZZ_SECONDS` | `60` | How long the test of each fuzz target runs with libFuzzer |
| `OPENXISF_FUZZING_ENGINE` | empty | Link the fuzz targets with this fuzzing engine (a flag such as `-fsanitize=fuzzer`, or a library) instead, with the instrumentation of `CMAKE_CXX_FLAGS`, as OSS-Fuzz builds them (`fuzz/oss-fuzz/build.sh`); no test runs them then |
| `OPENXISF_BUILD_DOCS` | `OFF` | Add the `docs` target, which builds the API documentation with Doxygen |
| `OPENXISF_INSTALL` | `ON` at the top level | Generate the install rules |
| `OPENXISF_ENABLE_SIMD` | `ON` | Shuffle and unshuffle bytes with SSE2 on x86-64; other processors use the portable code |
| `OPENXISF_WITH_TBB` | `OFF` | Use oneTBB to compress and decompress the subblocks of a block, and to convert large images between storage models, in parallel |
| `OPENXISF_WITH_OPENSSL` | `OFF` | Compute checksums with OpenSSL (libcrypto), several times faster than the built-in code |
| `OPENXISF_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors (CI turns it on) |
| `OPENXISF_HARDENING` | `OFF` | Compile with hardening flags (the presets turn it on) |
| `OPENXISF_SANITIZE` | empty | Sanitizers for the library and the tests: `address;undefined`, `thread` or `fuzzer;address;undefined` (Clang), or `address` (MSVC) |

Optional dependencies are never detected on their own. Everything except the library is off when OpenXISF is added
with `add_subdirectory` or `FetchContent`. The programs, and the DLL of a shared build, go to `bin` of the build
directory, unless `CMAKE_RUNTIME_OUTPUT_DIRECTORY` says otherwise, as the project that adds OpenXISF may.

## Using it from CMake

```cmake
find_package(openxisf CONFIG REQUIRED)
target_link_libraries(app PRIVATE openxisf::openxisf)
```

The installed package can be moved, unless it is installed with an absolute library directory (`CMAKE_INSTALL_LIBDIR`):
it then names the prefix of the configuration, which `cmake --install --prefix` cannot change. The library of a Debug
build has a `d` at the end of its name, so that Debug and Release builds can be installed into one prefix. A pkg-config
file, `openxisf.pc`, is installed as well, for the configuration installed last. OpenXISF is also set up to become a
vcpkg port.

The installed headers record whether the library is static or shared (`openxisf/linkage.h`), so a build system that
is given only the include and library directories, as a Visual Studio project or a makefile is, needs no definition to
link it; a static library also needs the libraries of its dependencies, which the CMake package and
`pkg-config --static` list. A prefix holds one of the two.

## Examples

A unit is opened with `openxisf::reader`, from a UTF-8 path or from a source. The reader describes each image, and reads
its pixel data on request, into memory of its own or of the application, as they are stored or interleaved. These
excerpts of `samples/read_pixels.cpp` open a unit, then read the samples of its first image as values of its sample
format, `T`, interleaved, while a progress function reports how far the read is:

```cpp
#include <openxisf/openxisf.h>

const openxisf::reader file(path);
```

```cpp
const openxisf::image_info& info = file.image(0);
std::vector<T> samples(info.geometry.sample_count());
file.read_pixels(0, std::span(samples),
                 {.storage = openxisf::pixel_storage::normal, .progress = progress_report()});
```

`samples/read_info.cpp` lists everything a unit holds: its metadata, its images with their properties, tables, FITS
keywords and the elements that describe them, and the problems the reader found in it, `file.diagnostics()`. The reader
loads the ICC profiles, the thumbnails and the values in data blocks when it opens a unit, within a configurable limit.
An application that only lists units can read their header alone, as `read_info --header-only` does, and load the rest
with `file.load_ancillary_data()` when it needs it:

```cpp
const bool header_only = arguments.size() == 3 && arguments[1] == "--header-only";
// Without the ancillary data, file.load_ancillary_data() would load them later.
const openxisf::reader file(arguments.back(), {.header_only = header_only});
```

`file.signature()` tells whether a unit is signed, and an application that verifies signatures gets the signature and
the root element that it covers from `file.signature_xml()` and `file.signed_xml()`.

An application shows an image through the algorithms of the specification. This excerpt of `samples/preview.cpp`
stretches the samples of an image, normalized to [0, 1] by its representable range, with its display function, or with
one that the adaptive algorithm computes from the statistics of its channels; a CIE L*a*b* image is converted to RGB
first, and the preview is turned to the orientation of the image, which is meant for showing it only:

```cpp
const bool adaptive = choice.adaptive || !info.display_function;
const openxisf::display_function function =
    adaptive
        ? openxisf::adaptive_display_function(std::as_bytes(std::span(samples)), info, {.linked = choice.linked})
        : info.display_function.value_or(openxisf::display_function{});
```

Each sample `x` of channel `channel` is then shown as `openxisf::apply_display_function(function, channel, x)`.
`read_info` writes property values with `openxisf::format_value()`, as the format specifier of each property asks.

An image may have an astrometric solution. `openxisf::astrometric_solution` reads it from the properties of the image,
says which of its layers are available and why the others are not, and converts between image and celestial
coordinates, in both directions, through the highest available layer. This excerpt of `samples/read_info.cpp` gives the
right ascension and declination of the centre of an image:

```cpp
const openxisf::astrometric_solution solution(info.properties);
const std::optional<openxisf::astrometric_layer> layer = solution.layer();
const std::optional<openxisf::astrometric_projection>& projection = solution.projection();
if (!layer || !projection) {
    if (solution.status(openxisf::astrometric_layer::linear) != openxisf::astrometric_status::absent) {
        std::cout << "  astrometric solution that cannot be used: "
                  << solution.problem(openxisf::astrometric_layer::linear) << '\n';
    }
    return;
}
```

```cpp
const openxisf::image_point centre{.x = static_cast<double>(size[0]) / 2.0,
                                   .y = size.size() > 1 ? static_cast<double>(size[1]) / 2.0 : 0.5};
if (const std::optional<openxisf::celestial_point> sky = solution.image_to_celestial(centre)) {
    std::cout << ", centre at RA " << number(sky->ra) << ", Dec " << number(sky->dec) << " ("
              << projection->celestial_reference_system << ")";
}
```

A solution describes the image it was computed for, and the writer keeps its properties as they are. An application
that crops, resamples or otherwise changes the geometry of an image removes them with
`openxisf::remove_astrometric_solution()`, as the specification requires.

A unit is written with `openxisf::writer`, from the same model that the reader returns, so that a unit read can be
written again; a unit read with `header_only` lacks its ancillary data, which the writer would lose, until
`load_ancillary_data()` loads them. This excerpt of `samples/write_image.cpp` writes an image with properties, FITS
keywords and a thumbnail, compressed with Zstandard and byte shuffling, with SHA-256 checksums; without these options
the writer compresses nothing and adds no checksums, which every XISF 1.0 decoder reads, and Zstandard needs a decoder
of Revision 1 of the specification:

```cpp
const std::vector<float> samples = render_sky();

openxisf::image_info info;
info.geometry = {.dimensions = {width, height}, .channels = 3};
info.sample_format = openxisf::sample_format::float32;
info.color_space = openxisf::color_space::rgb;
info.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
info.properties.set("Instrument:ExposureTime", 300.0F);
info.fits_keywords.push_back({.name = "EXPTIME", .value = "300.", .comment = "Exposure time in seconds"});
info.thumbnail = make_thumbnail(samples);

openxisf::writer output({.creator_application = "write_image sample 1.0",
                         .codec = openxisf::codec::zstd,
                         .byte_shuffle = true,
                         .checksum = openxisf::checksum_algorithm::sha256});
output.add_image(info, std::span(samples));
output.save(path);
```

The writer checks the whole unit before it writes anything, and throws `openxisf::validation_error`, which names the
element at fault, for anything that the specification forbids, but in an astrometric solution, whose properties it
checks only for their types and keeps as given. The pixel data are borrowed until the last `save()`, which can also
write to any `openxisf::output_sink`. `add_image()` takes them as bytes, or as a `std::span<const T>` of the type of
the sample format, which `std::span(samples)` gives here since `samples` is const; a span of samples that are not const
is given as `std::span<const float>(samples)`.

`save_distributed()` writes a distributed unit instead: a header file, and a data blocks file that the header locates
relative to its own directory, so that the two can move together. A unit opened from a path finds its data blocks files
in the directory of its header file and nowhere else; any other place, an absolute path, a URL or a store of the
application, is opened by a resolver of the application. This excerpt of `samples/distributed.cpp` writes a unit into
memory and reads it back with a resolver that finds its data blocks file there:

```cpp
openxisf::memory_sink header;
openxisf::memory_sink blocks;
output.save_distributed(header, blocks, "blocks/unit.xisb");
auto store = std::make_shared<std::map<std::string, std::vector<std::byte>>>();
(*store)["blocks/unit.xisb"] = blocks.release();

openxisf::read_options options;
options.resolver =
    [store](const openxisf::external_reference& reference) -> std::unique_ptr<openxisf::input_source> {
    std::cout << "  resolving " << reference.location << '\n';
    const auto found = store->find(reference.location);
    if (reference.form != openxisf::location_form::relative_path || found == store->end()) {
        return nullptr;
    }
    return std::make_unique<openxisf::memory_source>(std::span<const std::byte>(found->second));
};
const openxisf::reader from_memory(std::make_unique<openxisf::memory_source>(header.release()), options);
```

Units are read from an `openxisf::input_source` and written to an `openxisf::output_sink`. The library provides them for
files (paths are UTF-8 on every platform, and a file is replaced only when it has been written completely), for memory,
for functions of the application, and, in `<openxisf/stream_io.h>`, for C and C++ streams (`samples/stream_io.cpp`). An
application can also implement them over its own storage; this is an excerpt of `samples/custom_io.cpp`, a source over
the function table of a storage library written in C:

```cpp
#include <openxisf/openxisf.h>

class blob_source final : public openxisf::input_source
{
public:
    blob_source(const blob_functions& functions, const void* blob) : functions_(functions), blob_(blob) {}

    std::uint64_t size() const override
    {
        return functions_.size(blob_);
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (functions_.read_at(blob_, offset, destination.data(), destination.size()) != 0) {
            throw openxisf::io_error(openxisf::errc::read_failed, "cannot read the blob", {}, {.offset = offset});
        }
    }

private:
    blob_functions functions_;
    const void* blob_;
};
```

A source reads exactly the bytes it is asked for, at the offset it is given, or throws. The library never asks for bytes
beyond its size, and calls it from one thread at a time unless it declares concurrent reads.

## Thread safety, errors and text

These rules hold throughout the API.

- **Thread safety.** There is no mutable global state. The const member functions of a `reader` can be called from any
  number of threads at once, also to read pixels; `load_ancillary_data()`, the only one that changes it, needs exclusive
  access. An `astrometric_solution` is immutable, and any number of threads can use it at once. Writers and values are
  thread-compatible, like standard containers. The file and memory sources can be read from any number of threads at
  once; the library calls a source that does not declare concurrent reads from one thread at a time. Built with oneTBB,
  the library also works in parallel within one call, in the task arena of the calling thread and without changing the
  global settings of oneTBB; sources, sinks and progress functions are still called from the calling thread. That work
  is kept apart from the application's own oneTBB work: a cancelled task group of the application does not cancel it,
  and a thread that waits for it runs none of the application's tasks meanwhile.
- **Errors.** Failures are exceptions derived from `openxisf::error`, one class per kind of failure, each with an error
  code; besides them, `std::bad_alloc` when memory cannot be allocated, and the exceptions of the application's own
  sources, sinks, resolvers and progress functions, which pass through unchanged. A problem confined to one object of a
  unit, such as an unsupported codec, makes that object unavailable and is reported as a diagnostic, in
  `reader::diagnostics()`; the rest of the unit stays readable. Harmless deviations from the specification are
  diagnostics too; a reader lists at most 10,000, the last of which then counts the problems that follow.
  `read_options::strict` turns the problems that make an object unavailable into failures. A writer refuses a unit
  that violates the specification with `openxisf::validation_error`, before it writes anything. A long read or save can
  be cancelled from its progress function, which then throws `openxisf::cancelled_error`.
- **Text.** The API carries text as UTF-8 in `std::string` and `std::string_view`, paths included, on every platform.
  Invalid UTF-8 is rejected.

## Security

Report a vulnerability through GitHub's private vulnerability reporting, on the Security tab of the repository. Please do
not open a public issue for it.

Files are untrusted input. The design goals are: every size and offset is overflow-checked and every block is
bounds-checked; allocations are limited and configurable, and so are the size of the XML header, the nesting of its
elements and their number, and the window of Zstandard data; checksums are verified before data is used, compressed
data included, which are never decompressed when their checksum fails; decompression produces exactly the declared size
or fails, and a compressed block cannot declare more than its codec can decode from its bytes; a document type
declaration in the XML header is rejected; there is no network access; the paths of a distributed unit stay inside the
directory of its header file, once symbolic links are followed, and open no device, and absolute paths and URLs are
opened only by a resolver of the application. Inside is a matter of paths: a hard link or a mount point in that
directory counts as inside. These goals are checked continuously by fuzzing, sanitizers and static analysis. The size
of the header bounds the memory that an open takes, up to about 40 times for a header made of `Image` elements
(`limits.h` gives the details), and a reader lists at most 10,000 diagnostics.

## Contributing

- Tests come first, with GoogleTest. Each behaviour is tested once, at the lowest level that proves it.
- A change to a parser, a codec or the I/O layer adds or extends a fuzz target, or adds a seed to `fuzz/seeds`. An input
  that once broke a target becomes a seed.
- Write plain code: classes and free functions, templates only where they remove real duplication. The naming follows
  the standard library: `snake_case` everywhere, lowercase enumerators, a trailing `_` on private members.
- Public headers carry short Doxygen comments. Every source file starts with an SPDX header, as the existing ones do;
  a file you create names you in its `SPDX-FileCopyrightText` line, and a file you change substantially can get a line
  of its own for you.
- Format with clang-format 22 and check with clang-tidy 22 (`pip install clang-format==22.1.3 clang-tidy==22.1.8`).
  Both run in CI.
- Sign off your commits to certify the [Developer Certificate of Origin](https://developercertificate.org/):
  `git commit -s`.
- Do not copy code from other XISF implementations that are under the GPL or the LGPL, or from the PixInsight Class
  Library.
- A pull request needs a green CI: every platform builds with warnings as errors, and the tests pass, also under the
  sanitizers.

## Author

OpenXISF is written and maintained by Ezequiel Ruiz ([@emruiz81](https://github.com/emruiz81)). Contributors are
credited in the history of the repository and in the SPDX headers of the files they wrote.

## License

OpenXISF is licensed under the [Apache License 2.0](LICENSE); see also [NOTICE](NOTICE).

OpenXISF is an independent implementation. It is not affiliated with or endorsed by Pleiades Astrophoto S.L.
(PixInsight).
