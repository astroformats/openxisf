// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The main program of a fuzz target built without libFuzzer. It runs the target once on every file given on the command
// line and on every file of the directories given, so that the committed seeds, which include every input that ever
// broke a target, run on every platform. A crash or an escaping exception fails the run.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
// shellapi.h needs the declarations of windows.h before it, so the two keep this order.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

// The name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

// The arguments after the name of the program. On Windows, main() receives them in the ANSI code page, which cannot
// represent every character of a path, so they are taken from CommandLineToArgvW() in UTF-16.
std::vector<std::filesystem::path> arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if defined(_WIN32)
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "cannot read the command line");
    }
    std::vector<std::filesystem::path> result;
    try {
        for (int i = 1; i < count; ++i) {
            result.emplace_back(wide[i]);
        }
    } catch (...) {
        LocalFree(static_cast<HLOCAL>(wide));
        throw;
    }
    LocalFree(static_cast<HLOCAL>(wide));
    return result;
#else
    return {argv + 1, argv + argc};
#endif
}

std::vector<std::filesystem::path> list_inputs(int argc, char** argv)
{
    std::vector<std::filesystem::path> inputs;
    for (const std::filesystem::path& argument : arguments(argc, argv)) {
        if (std::filesystem::is_directory(argument)) {
            for (const auto& entry : std::filesystem::directory_iterator(argument)) {
                if (entry.is_regular_file()) {
                    inputs.push_back(entry.path());
                }
            }
        } else {
            inputs.push_back(argument);
        }
    }
    std::ranges::sort(inputs);
    return inputs;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::filesystem::filesystem_error("cannot open the input", path,
                                                std::make_error_code(std::errc::io_error));
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        throw std::filesystem::filesystem_error("cannot read the input", path,
                                                std::make_error_code(std::errc::io_error));
    }
    return data;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::filesystem::path> inputs = list_inputs(argc, argv);
        // A wrong path must not pass as a run without findings.
        if (inputs.empty()) {
            (void)std::fputs("no inputs; give files or directories\n", stderr);
            return 1;
        }
        for (const std::filesystem::path& input : inputs) {
            const std::vector<std::uint8_t> data = read_file(input);
            // Printed before the run, so that the input of a crash is the last one named; in UTF-8, which any name
            // has, where the narrow string of a path on Windows is in the ANSI code page.
            const std::u8string name = input.filename().u8string();
            std::cout.write(reinterpret_cast<const char*>(name.data()), static_cast<std::streamsize>(name.size()));
            std::cout << '\n';
            std::cout.flush();
            (void)LLVMFuzzerTestOneInput(data.data(), data.size());
        }
        std::cout << "replayed " << inputs.size() << " inputs\n";
        return 0;
    } catch (const std::exception& failure) {
        // Also an exception that escapes the target, which fails the run as it does under libFuzzer.
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
