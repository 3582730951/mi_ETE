#pragma once

#include <string>
#include <vector>

#include "server/config.hpp"

namespace mi::server
{
class AuthService
{
public:
    AuthService();
    explicit AuthService(const std::vector<UserCredential>& allowed);

    bool Validate(const std::wstring& username, const std::wstring& password) const;
    void SetAllowedUsers(const std::vector<UserCredential>& allowed);

private:
    std::vector<UserCredential> allowedUsers_;
};
}  // namespace mi::server
