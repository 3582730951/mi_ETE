#include <cassert>
#include <string>

#include "client/secure_types.hpp"

int main()
{
    mi::client::SecureInt8 i8(-12);
    assert(i8.Value() == -12);
    i8.Set(45);
    assert(i8.Value() == 45);

    mi::client::SecureUInt8 u8(250u);
    assert(u8.Value() == 250u);
    u8.Set(1u);
    assert(u8.Value() == 1u);

    mi::client::SecureInt16 i16(-12345);
    assert(i16.Value() == -12345);
    i16.Set(1234);
    assert(i16.Value() == 1234);

    mi::client::SecureUInt16 u16(54321u);
    assert(u16.Value() == 54321u);
    u16.Set(99u);
    assert(u16.Value() == 99u);

    mi::client::SecureInt32 value(-123456);
    assert(value.Value() == -123456);

    value.Set(98765);
    assert(value.Value() == 98765);

    mi::client::SecureUInt32 u32(123456u);
    assert(u32.Value() == 123456u);
    u32.Set(987654u);
    assert(u32.Value() == 987654u);

    mi::client::SecureInt64 i64(-1234567890123LL);
    assert(i64.Value() == -1234567890123LL);
    i64.Set(2222222LL);
    assert(i64.Value() == 2222222LL);

    mi::client::SecureUInt64 u64(999999999ULL);
    assert(u64.Value() == 999999999ULL);
    u64.Set(1ULL);
    assert(u64.Value() == 1ULL);

    mi::client::SecureShort sh(-123);
    assert(sh.Value() == -123);
    sh.Set(456);
    assert(sh.Value() == 456);

    mi::client::SecureUShort ush(321u);
    assert(ush.Value() == 321u);
    ush.Set(654u);
    assert(ush.Value() == 654u);

    mi::client::SecureLong lng(-98765);
    assert(lng.Value() == -98765);
    lng.Set(123456);
    assert(lng.Value() == 123456);

    mi::client::SecureULong ulng(3000000000UL);
    assert(ulng.Value() == 3000000000UL);
    ulng.Set(42UL);
    assert(ulng.Value() == 42UL);

    mi::client::SecureFloat f32(3.14f);
    assert(f32.Value() > 3.13f && f32.Value() < 3.15f);
    f32.Set(-1.5f);
    assert(f32.Value() < -1.4f && f32.Value() > -1.6f);

    mi::client::SecureDouble f64(6.28);
    assert(f64.Value() > 6.27 && f64.Value() < 6.29);
    f64.Set(0.001);
    assert(f64.Value() > 0.0009 && f64.Value() < 0.0011);

    mi::client::SecureBool flag(true);
    assert(flag.Value());
    flag.Set(false);
    assert(!flag.Value());

    mi::client::SecureChar ch('a');
    assert(ch.Value() == 'a');
    ch.Set('z');
    assert(ch.Value() == 'z');

    mi::client::SecureWChar wch(L'b');
    assert(wch.Value() == L'b');
    wch.Set(L'x');
    assert(wch.Value() == L'x');

    mi::client::SecureSize sz(123);
    assert(sz.Value() == 123);
    sz.Set(999);
    assert(sz.Value() == 999);

    mi::client::SecureString text(L"client secure string test");
    assert(text.Value() == L"client secure string test");

    text.Set(L"hello");
    assert(text.Value() == L"hello");

    return 0;
}
