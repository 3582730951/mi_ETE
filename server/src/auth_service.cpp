#include "server/auth_service.hpp"

namespace mi::server
{
AuthService::AuthService() : allowedUsers_()
{
}

AuthService::AuthService(const std::vector<UserCredential>& allowed) : allowedUsers_(allowed)
{
}

void AuthService::SetAllowedUsers(const std::vector<UserCredential>& allowed)
{
    allowedUsers_ = allowed;
}

bool AuthService::Validate(const std::wstring& username, const std::wstring& password) const
{
    if (username.empty() || password.empty())
    {
        return false;
    }

    if (allowedUsers_.empty())
    {
        return true;
    }

    for (const auto& user : allowedUsers_)
    {
        if (user.username == username && user.password == password)
        {
            return true;
        }
    }
    return false;
}
}  // namespace mi::server
