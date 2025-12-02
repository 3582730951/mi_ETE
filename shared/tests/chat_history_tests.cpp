#include <cassert>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "mi/shared/storage/chat_history.hpp"

int main()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "mi_chat_test";
    std::vector<std::uint8_t> rootKey = {1, 2, 3, 4, 5, 6, 7, 8};
    mi::shared::storage::ChatHistoryStore store(root, rootKey);

    std::vector<std::uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    std::vector<std::uint8_t> dynKey = {9, 8, 7, 6};

    const auto rec = store.Append(42, L"bob", payload, {dynKey});
    assert(rec.id != 0);
    assert(store.Exists(rec.id));

    mi::shared::storage::ChatRecord loaded{};
    assert(store.Load(rec.id, dynKey, loaded));
    assert(loaded.sessionId == 42);
    assert(loaded.payload == payload);

    assert(store.Revoke(rec.id));
    assert(!store.Exists(rec.id));
    return 0;
}
