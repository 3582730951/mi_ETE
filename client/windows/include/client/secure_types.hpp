#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <codecvt>
#include <locale>
#include <random>
#include <string>
#include <vector>

namespace mi::client
{
namespace detail
{
template <std::size_t N>
struct Permutations;

template <>
struct Permutations<1>
{
    static constexpr std::array<std::array<int, 1>, 1> kValues = {{{0}}};
};

template <>
struct Permutations<2>
{
    static constexpr std::array<std::array<int, 2>, 2> kValues = {{{0, 1}, {1, 0}}};
};

template <>
struct Permutations<4>
{
    static constexpr std::array<std::array<int, 4>, 24> kValues = {{
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {0, 3, 2, 1},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
        {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0}, {2, 3, 0, 1}, {2, 3, 1, 0},
        {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
    }};
};

template <>
struct Permutations<8>
{
    static constexpr std::array<std::array<int, 8>, 24> kValues = {{
        {0, 1, 2, 3, 4, 5, 6, 7}, {1, 0, 2, 3, 4, 5, 6, 7}, {2, 3, 0, 1, 4, 5, 6, 7}, {3, 2, 1, 0, 4, 5, 6, 7},
        {4, 5, 6, 7, 0, 1, 2, 3}, {5, 4, 6, 7, 0, 1, 2, 3}, {6, 7, 4, 5, 0, 1, 2, 3}, {7, 6, 5, 4, 0, 1, 2, 3},
        {0, 2, 4, 6, 1, 3, 5, 7}, {1, 3, 5, 7, 0, 2, 4, 6}, {2, 4, 6, 0, 3, 5, 7, 1}, {3, 5, 7, 1, 2, 4, 6, 0},
        {0, 1, 4, 5, 2, 3, 6, 7}, {1, 0, 5, 4, 3, 2, 7, 6}, {2, 3, 6, 7, 0, 1, 4, 5}, {3, 2, 7, 6, 1, 0, 5, 4},
        {4, 0, 5, 1, 6, 2, 7, 3}, {5, 1, 4, 0, 7, 3, 6, 2}, {6, 2, 7, 3, 4, 0, 5, 1}, {7, 3, 6, 2, 5, 1, 4, 0},
        {0, 3, 6, 1, 4, 7, 2, 5}, {1, 2, 7, 0, 5, 6, 3, 4}, {2, 5, 0, 7, 6, 1, 4, 3}, {3, 4, 1, 6, 7, 2, 5, 0},
    }};
};

template <std::size_t N>
std::size_t SelectPermutationIndex()
{
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::size_t> distribution(0, Permutations<N>::kValues.size() - 1);
    return distribution(generator);
}

template <std::size_t N>
std::uint8_t DeriveKey(std::size_t index)
{
    return static_cast<std::uint8_t>((index * 37u + 0x5Au) & 0xFFu);
}

template <typename T>
std::array<std::uint8_t, sizeof(T)> ToBytes(const T& value)
{
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T, std::size_t N = sizeof(T)>
class SecureValue
{
public:
    SecureValue() : data_{} { Scramble(static_cast<T>(0)); }
    explicit SecureValue(T value) : data_{} { Scramble(value); }

    void Set(T value) { Scramble(value); }
    T Value() const { return Restore(); }

private:
    void Scramble(T value)
    {
        const auto bytes = ToBytes(value);
        const std::size_t index = SelectPermutationIndex<N>();
        const auto& permutation = Permutations<N>::kValues[index];
        const std::uint8_t key = DeriveKey<N>(index);

        data_[0] = static_cast<std::uint8_t>(index);
        for (std::size_t i = 0; i < N; ++i)
        {
            const int sourceIndex = permutation[i];
            data_[i + 1] = static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(sourceIndex)] ^ key);
        }
    }

    T Restore() const
    {
        const std::size_t index =
            (data_[0] < Permutations<N>::kValues.size()) ? static_cast<std::size_t>(data_[0]) : 0;
        const auto& permutation = Permutations<N>::kValues[index];
        const std::uint8_t key = DeriveKey<N>(index);

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
};

inline std::wstring Utf8ToWide(const std::string& text)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(text);
}

inline std::string WideToUtf8(const std::wstring& text)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.to_bytes(text);
}
}  // namespace detail

class SecureInt8
{
public:
    SecureInt8();
    explicit SecureInt8(std::int8_t value);

    void Set(std::int8_t value);
    std::int8_t Value() const;

private:
    detail::SecureValue<std::int8_t, 1> value_;
};

class SecureUInt8
{
public:
    SecureUInt8();
    explicit SecureUInt8(std::uint8_t value);

    void Set(std::uint8_t value);
    std::uint8_t Value() const;

private:
    detail::SecureValue<std::uint8_t, 1> value_;
};

class SecureInt16
{
public:
    SecureInt16();
    explicit SecureInt16(std::int16_t value);

    void Set(std::int16_t value);
    std::int16_t Value() const;

private:
    detail::SecureValue<std::int16_t, sizeof(std::int16_t)> value_;
};

class SecureUInt16
{
public:
    SecureUInt16();
    explicit SecureUInt16(std::uint16_t value);

    void Set(std::uint16_t value);
    std::uint16_t Value() const;

private:
    detail::SecureValue<std::uint16_t, sizeof(std::uint16_t)> value_;
};

class SecureInt32
{
public:
    SecureInt32();
    explicit SecureInt32(std::int32_t value);

    void Set(std::int32_t value);
    std::int32_t Value() const;

private:
    detail::SecureValue<std::int32_t> value_;
};

class SecureUInt32
{
public:
    SecureUInt32();
    explicit SecureUInt32(std::uint32_t value);

    void Set(std::uint32_t value);
    std::uint32_t Value() const;

private:
    detail::SecureValue<std::uint32_t> value_;
};

class SecureInt64
{
public:
    SecureInt64();
    explicit SecureInt64(std::int64_t value);

    void Set(std::int64_t value);
    std::int64_t Value() const;

private:
    detail::SecureValue<std::int64_t> value_;
};

class SecureUInt64
{
public:
    SecureUInt64();
    explicit SecureUInt64(std::uint64_t value);

    void Set(std::uint64_t value);
    std::uint64_t Value() const;

private:
    detail::SecureValue<std::uint64_t> value_;
};

class SecureShort
{
public:
    SecureShort();
    explicit SecureShort(short value);

    void Set(short value);
    short Value() const;

private:
    detail::SecureValue<short, sizeof(short)> value_;
};

class SecureUShort
{
public:
    SecureUShort();
    explicit SecureUShort(unsigned short value);

    void Set(unsigned short value);
    unsigned short Value() const;

private:
    detail::SecureValue<unsigned short, sizeof(unsigned short)> value_;
};

class SecureLong
{
public:
    SecureLong();
    explicit SecureLong(long value);

    void Set(long value);
    long Value() const;

private:
    detail::SecureValue<long, sizeof(long)> value_;
};

class SecureULong
{
public:
    SecureULong();
    explicit SecureULong(unsigned long value);

    void Set(unsigned long value);
    unsigned long Value() const;

private:
    detail::SecureValue<unsigned long, sizeof(unsigned long)> value_;
};

class SecureFloat
{
public:
    SecureFloat();
    explicit SecureFloat(float value);

    void Set(float value);
    float Value() const;

private:
    detail::SecureValue<float> value_;
};

class SecureDouble
{
public:
    SecureDouble();
    explicit SecureDouble(double value);

    void Set(double value);
    double Value() const;

private:
    detail::SecureValue<double> value_;
};

class SecureString
{
public:
    SecureString();
    explicit SecureString(const std::wstring& text);

    void Set(const std::wstring& text);
    std::wstring Value() const;

private:
    std::vector<std::uint8_t> buffer_;
};

class SecureBool
{
public:
    SecureBool();
    explicit SecureBool(bool value);

    void Set(bool value);
    bool Value() const;

private:
    detail::SecureValue<std::uint8_t, 1> value_;
};

class SecureChar
{
public:
    SecureChar();
    explicit SecureChar(char value);

    void Set(char value);
    char Value() const;

private:
    detail::SecureValue<char, 1> value_;
};

class SecureWChar
{
public:
    SecureWChar();
    explicit SecureWChar(wchar_t value);

    void Set(wchar_t value);
    wchar_t Value() const;

private:
    detail::SecureValue<wchar_t, sizeof(wchar_t)> value_;
};

class SecureSize
{
public:
    SecureSize();
    explicit SecureSize(std::size_t value);

    void Set(std::size_t value);
    std::size_t Value() const;

private:
    detail::SecureValue<std::size_t, sizeof(std::size_t)> value_;
};

using SecureInt = SecureInt32;
using SecureUInt = SecureUInt32;
using SecureLongLong = SecureInt64;
using SecureULongLong = SecureUInt64;
}  // namespace mi::client
