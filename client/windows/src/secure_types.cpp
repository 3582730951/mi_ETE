#include "client/secure_types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <codecvt>
#include <cstddef>
#include <locale>
#include <string>
#include <utility>

namespace
{
}  // namespace

namespace mi::client
{
SecureInt8::SecureInt8() : value_(0)
{
}

SecureInt8::SecureInt8(std::int8_t value) : value_(value)
{
}

void SecureInt8::Set(std::int8_t value)
{
    value_.Set(value);
}

std::int8_t SecureInt8::Value() const
{
    return value_.Value();
}

SecureUInt8::SecureUInt8() : value_(0)
{
}

SecureUInt8::SecureUInt8(std::uint8_t value) : value_(value)
{
}

void SecureUInt8::Set(std::uint8_t value)
{
    value_.Set(value);
}

std::uint8_t SecureUInt8::Value() const
{
    return value_.Value();
}

SecureInt16::SecureInt16() : value_(0)
{
}

SecureInt16::SecureInt16(std::int16_t value) : value_(value)
{
}

void SecureInt16::Set(std::int16_t value)
{
    value_.Set(value);
}

std::int16_t SecureInt16::Value() const
{
    return value_.Value();
}

SecureUInt16::SecureUInt16() : value_(0)
{
}

SecureUInt16::SecureUInt16(std::uint16_t value) : value_(value)
{
}

void SecureUInt16::Set(std::uint16_t value)
{
    value_.Set(value);
}

std::uint16_t SecureUInt16::Value() const
{
    return value_.Value();
}

SecureInt32::SecureInt32() : value_()
{
}

SecureInt32::SecureInt32(std::int32_t value) : value_(value)
{
}

void SecureInt32::Set(std::int32_t value)
{
    value_.Set(value);
}

std::int32_t SecureInt32::Value() const
{
    return value_.Value();
}

SecureUInt32::SecureUInt32() : value_()
{
}

SecureUInt32::SecureUInt32(std::uint32_t value) : value_(value)
{
}

void SecureUInt32::Set(std::uint32_t value)
{
    value_.Set(value);
}

std::uint32_t SecureUInt32::Value() const
{
    return value_.Value();
}

SecureInt64::SecureInt64() : value_()
{
}

SecureInt64::SecureInt64(std::int64_t value) : value_(value)
{
}

void SecureInt64::Set(std::int64_t value)
{
    value_.Set(value);
}

std::int64_t SecureInt64::Value() const
{
    return value_.Value();
}

SecureUInt64::SecureUInt64() : value_()
{
}

SecureUInt64::SecureUInt64(std::uint64_t value) : value_(value)
{
}

void SecureUInt64::Set(std::uint64_t value)
{
    value_.Set(value);
}

std::uint64_t SecureUInt64::Value() const
{
    return value_.Value();
}

SecureShort::SecureShort() : value_(0)
{
}

SecureShort::SecureShort(short value) : value_(value)
{
}

void SecureShort::Set(short value)
{
    value_.Set(value);
}

short SecureShort::Value() const
{
    return value_.Value();
}

SecureUShort::SecureUShort() : value_(0)
{
}

SecureUShort::SecureUShort(unsigned short value) : value_(value)
{
}

void SecureUShort::Set(unsigned short value)
{
    value_.Set(value);
}

unsigned short SecureUShort::Value() const
{
    return value_.Value();
}

SecureLong::SecureLong() : value_(0)
{
}

SecureLong::SecureLong(long value) : value_(value)
{
}

void SecureLong::Set(long value)
{
    value_.Set(value);
}

long SecureLong::Value() const
{
    return value_.Value();
}

SecureULong::SecureULong() : value_(0)
{
}

SecureULong::SecureULong(unsigned long value) : value_(value)
{
}

void SecureULong::Set(unsigned long value)
{
    value_.Set(value);
}

unsigned long SecureULong::Value() const
{
    return value_.Value();
}

SecureFloat::SecureFloat() : value_(0.0f)
{
}

SecureFloat::SecureFloat(float value) : value_(value)
{
}

void SecureFloat::Set(float value)
{
    value_.Set(value);
}

float SecureFloat::Value() const
{
    return value_.Value();
}

SecureDouble::SecureDouble() : value_(0.0)
{
}

SecureDouble::SecureDouble(double value) : value_(value)
{
}

void SecureDouble::Set(double value)
{
    value_.Set(value);
}

double SecureDouble::Value() const
{
    return value_.Value();
}

SecureString::SecureString() : buffer_()
{
}

SecureString::SecureString(const std::wstring& text) : buffer_()
{
    Set(text);
}

void SecureString::Set(const std::wstring& text)
{
    const std::string utf8 = detail::WideToUtf8(text);
    if (utf8.empty())
    {
        buffer_.clear();
        return;
    }

    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(1, 254);
    const std::uint8_t mode = static_cast<std::uint8_t>(dist(gen));
    const std::uint8_t key = static_cast<std::uint8_t>((utf8.size() * 31u + 0x7Du + mode) & 0xFFu);

    buffer_.clear();
    buffer_.reserve(utf8.size() + 1);
    buffer_.push_back(mode);

    for (std::size_t i = 0; i < utf8.size(); ++i)
    {
        const std::uint8_t mixed = static_cast<std::uint8_t>(utf8[i] ^ key ^ static_cast<std::uint8_t>(mode + i));
        buffer_.push_back(mixed);
    }
    std::reverse(buffer_.begin() + 1, buffer_.end());
}

std::wstring SecureString::Value() const
{
    if (buffer_.empty())
    {
        return L"";
    }

    const std::uint8_t mode = buffer_[0];
    if (mode == 0 || buffer_.size() == 1)
    {
        return L"";
    }

    std::vector<std::uint8_t> restored(buffer_.begin() + 1, buffer_.end());
    std::reverse(restored.begin(), restored.end());
    const std::uint8_t key =
        static_cast<std::uint8_t>(((restored.size()) * 31u + 0x7Du + mode) & 0xFFu);

    for (std::size_t i = 0; i < restored.size(); ++i)
    {
        restored[i] = static_cast<std::uint8_t>(restored[i] ^ key ^ static_cast<std::uint8_t>(mode + i));
    }

    const std::string utf8(restored.begin(), restored.end());
    return detail::Utf8ToWide(utf8);
}

SecureBool::SecureBool() : value_(0)
{
}

SecureBool::SecureBool(bool value) : value_(value ? 1u : 0u)
{
}

void SecureBool::Set(bool value)
{
    value_.Set(value ? 1u : 0u);
}

bool SecureBool::Value() const
{
    return value_.Value() != 0;
}

SecureChar::SecureChar() : value_(0)
{
}

SecureChar::SecureChar(char value) : value_(value)
{
}

void SecureChar::Set(char value)
{
    value_.Set(value);
}

char SecureChar::Value() const
{
    return value_.Value();
}

SecureWChar::SecureWChar() : value_(0)
{
}

SecureWChar::SecureWChar(wchar_t value) : value_(value)
{
}

void SecureWChar::Set(wchar_t value)
{
    value_.Set(value);
}

wchar_t SecureWChar::Value() const
{
    return value_.Value();
}

SecureSize::SecureSize() : value_(0)
{
}

SecureSize::SecureSize(std::size_t value) : value_(value)
{
}

void SecureSize::Set(std::size_t value)
{
    value_.Set(value);
}

std::size_t SecureSize::Value() const
{
    return value_.Value();
}
}  // namespace mi::client
