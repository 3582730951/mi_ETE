#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "mi/shared/storage/disordered_file.hpp"

namespace fs = std::filesystem;
using mi::shared::storage::DisorderedFileStore;
using mi::shared::storage::DisorderedOptions;

int main()
{
    const fs::path tempDir = fs::temp_directory_path() / "mi_disordered_tests";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir);

    DisorderedFileStore store(tempDir, {0x11u, 0x22u, 0x33u});
    auto fail = [&](int code) {
        std::wcerr << L"[disordered_test] step " << code << L" failed\n";
        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        return code;
    };

    std::vector<std::uint8_t> data(513);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<std::uint8_t>(i & 0xFFu);
    }

    DisorderedOptions opts;
    opts.chunkSize = 64;
    opts.seed = 12345;

    try
    {
        const auto saved = store.Save(L"picture.png", data, {0x9Au, 0xBCu, 0xDEu}, opts);
        if (!store.Exists(saved.id))
        {
            return fail(1);
        }

        std::vector<std::uint8_t> restored;
        const bool loaded = store.Load(saved.id, {0x9Au, 0xBCu, 0xDEu}, restored);
        if (!loaded || restored != data)
        {
            return fail(2);
        }

        std::vector<std::uint8_t> wrongKeyContent;
        const bool wrongKey = store.Load(saved.id, {0x01u}, wrongKeyContent);
        if (wrongKey)
        {
            return fail(3);
        }

        std::ifstream raw(saved.path, std::ios::binary);
        std::vector<std::uint8_t> rawBytes((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
        if (rawBytes.empty() || rawBytes.size() <= data.size())
        {
            return fail(4);
        }
        const std::size_t compare = std::min<std::size_t>(16, data.size());
        const bool startsWithPlain = std::equal(data.begin(), data.begin() + compare, rawBytes.begin());
        if (startsWithPlain)
        {
            return fail(5);
        }

        const auto second = store.Save(L"video.mp4", std::vector<std::uint8_t>(128, 0xABu), {0xCDu}, opts);
        if (!store.Exists(second.id))
        {
            return fail(6);
        }
        const bool revoked = store.Revoke(second.id);
        if (!revoked || store.Exists(second.id))
        {
            return fail(7);
        }

        if (!DisorderedFileStore::IsSupportedMediaExtension(L"file.jpg") ||
            DisorderedFileStore::IsSupportedMediaExtension(L"file.txt"))
        {
            return fail(8);
        }

        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::wcerr << L"[disordered_test] exception: " << ex.what() << L"\n";
        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        return fail(9);
    }
    catch (...)
    {
        std::wcerr << L"[disordered_test] unknown exception\n";
        std::error_code cleanupEc;
        fs::remove_all(tempDir, cleanupEc);
        return fail(9);
    }
}
