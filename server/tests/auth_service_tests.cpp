#include <cassert>
#include <string>
#include <vector>

#include "server/auth_service.hpp"

int main()
{
    using mi::server::AuthService;
    using mi::server::UserCredential;

    std::vector<UserCredential> users{{L"user1", L"pass1"}, {L"user2", L"pass2"}};
    AuthService strict(users);
    assert(strict.Validate(L"user1", L"pass1"));
    assert(strict.Validate(L"user2", L"pass2"));
    assert(!strict.Validate(L"user1", L"wrong"));
    assert(!strict.Validate(L"unknown", L"pass1"));
    assert(!strict.Validate(L"", L"pass1"));

    AuthService allowAny;
    assert(allowAny.Validate(L"any", L"nonempty"));
    assert(!allowAny.Validate(L"", L""));

    AuthService late;
    late.SetAllowedUsers(users);
    assert(late.Validate(L"user1", L"pass1"));
    assert(!late.Validate(L"user1", L"bad"));
    return 0;
}
