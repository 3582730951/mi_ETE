#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <type_traits>

namespace mi::shared::secure
{
namespace detail
{
template <std::size_t N>
struct Permutations;

template <>
struct Permutations<1>
{
    static constexpr std::array<std::array<std::uint8_t, 1>, 1> kValues = {{{0}}};
};

template <>
struct Permutations<2>
{
    static constexpr std::array<std::array<std::uint8_t, 2>, 2> kValues = {{{0, 1}, {1, 0}}};
};

// 4 与 8 字节对应常见的 32/64 位基础类型
template <>
struct Permutations<4>
{
    static constexpr std::array<std::array<std::uint8_t, 4>, 24> kValues = {{
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {0, 3, 2, 1},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
        {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0}, {2, 3, 0, 1}, {2, 3, 1, 0},
        {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
    }};
};

template <>
struct Permutations<8>
{
    static constexpr std::array<std::array<std::uint8_t, 8>, 24> kValues = {{
        {0, 1, 2, 3, 4, 5, 6, 7}, {1, 0, 2, 3, 4, 5, 6, 7}, {2, 3, 0, 1, 4, 5, 6, 7}, {3, 2, 1, 0, 4, 5, 6, 7},
        {4, 5, 6, 7, 0, 1, 2, 3}, {5, 4, 6, 7, 0, 1, 2, 3}, {6, 7, 4, 5, 0, 1, 2, 3}, {7, 6, 5, 4, 0, 1, 2, 3},
        {0, 2, 4, 6, 1, 3, 5, 7}, {1, 3, 5, 7, 0, 2, 4, 6}, {2, 4, 6, 0, 3, 5, 7, 1}, {3, 5, 7, 1, 2, 4, 6, 0},
        {0, 1, 4, 5, 2, 3, 6, 7}, {1, 0, 5, 4, 3, 2, 7, 6}, {2, 3, 6, 7, 0, 1, 4, 5}, {3, 2, 7, 6, 1, 0, 5, 4},
        {4, 0, 5, 1, 6, 2, 7, 3}, {5, 1, 4, 0, 7, 3, 6, 2}, {6, 2, 7, 3, 4, 0, 5, 1}, {7, 3, 6, 2, 5, 1, 4, 0},
        {0, 3, 6, 1, 4, 7, 2, 5}, {1, 2, 7, 0, 5, 6, 3, 4}, {2, 5, 0, 7, 6, 1, 4, 3}, {3, 4, 1, 6, 7, 2, 5, 0},
    }};
};

template <std::size_t N>
inline std::size_t SelectPermutationIndex()
{
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::size_t> distribution(0, Permutations<N>::kValues.size() - 1);
    return distribution(generator);
}

template <std::size_t N>
inline std::uint8_t DeriveKey(std::size_t index, std::uint8_t salt)
{
    return static_cast<std::uint8_t>((index * 37u + 0x5Au + salt) & 0xFFu);
}

template <typename T>
inline std::array<std::uint8_t, sizeof(T)> ToBytes(const T& value)
{
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}
}  // namespace detail

// 基础安全类型：通过随机字节排列+异或掩码+单字节盐混淆内存布局。
// N 默认为 sizeof(T)，可覆盖以缩小/扩展混淆空间。
template <typename T, std::size_t N = sizeof(T)>
class ObfuscatedValue
{
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    static_assert(N == 1 || N == 2 || N == 4 || N == 8, "Unsupported width for obfuscation");

public:
    ObfuscatedValue() : data_{}, salt_(RandomSalt()) { Scramble(static_cast<T>(0)); }
    explicit ObfuscatedValue(T value) : data_{}, salt_(RandomSalt()) { Scramble(value); }

    void Set(T value) { Scramble(value); }

    T Value() const { return Restore(); }

    T Increment(T step = 1)
    {
        const T next = static_cast<T>(Restore() + step);
        Scramble(next);
        return next;
    }

    T FetchAndIncrement(T step = 1)
    {
        const T current = Restore();
        Scramble(static_cast<T>(current + step));
        return current;
    }

private:
    static std::uint8_t RandomSalt()
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(1, 0xFF);
        return static_cast<std::uint8_t>(dist(rng));
    }

    void Scramble(T value)
    {
        const auto bytes = detail::ToBytes(value);
        const std::size_t index = detail::SelectPermutationIndex<N>();
        const auto& permutation = detail::Permutations<N>::kValues[index];
        const std::uint8_t key = detail::DeriveKey<N>(index, salt_);

        data_[0] = static_cast<std::uint8_t>(index ^ salt_);
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::uint8_t sourceIndex = permutation[i];
            data_[i + 1] = static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(sourceIndex)] ^ key);
        }
    }

    T Restore() const
    {
        const std::size_t indexCandidate = static_cast<std::size_t>(data_[0] ^ salt_);
        const std::size_t index =
            (indexCandidate < detail::Permutations<N>::kValues.size()) ? indexCandidate : 0u;
        const auto& permutation = detail::Permutations<N>::kValues[index];
        const std::uint8_t key = detail::DeriveKey<N>(index, salt_);

        std::array<std::uint8_t, N> bytes{};
        for (std::size_t i = 0; i < N; ++i)
        {
            const auto targetIndex = static_cast<std::size_t>(permutation[i]);
            bytes[targetIndex] = static_cast<std::uint8_t>(data_[i + 1] ^ key);
        }

        T value{};
        std::memcpy(&value, bytes.data(), N);
        return value;
    }

    std::array<std::uint8_t, N + 1> data_;
    std::uint8_t salt_;
};

using ObfuscatedUint32 = ObfuscatedValue<std::uint32_t>;
using ObfuscatedUint64 = ObfuscatedValue<std::uint64_t>;
using ObfuscatedInt32 = ObfuscatedValue<std::int32_t>;
using ObfuscatedSize = ObfuscatedValue<std::size_t, sizeof(std::size_t)>;
}  // namespace mi::shared::secure
