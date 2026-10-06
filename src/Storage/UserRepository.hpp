#pragma once

#include <QSqlDatabase>
#include <optional>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Storage {

class UserRepository : public BaseRepository
{
public:
    UserRepository(Core::Snowflake accountId);

    void saveUser(const Discord::User &user);
    void saveUser(const Discord::User &user, QSqlDatabase &db);
    void saveUsers(const QList<Discord::User> &users);
    void saveUsers(const QList<Discord::User> &users, QSqlDatabase &db);

    std::optional<Discord::User> getUser(Core::Snowflake userId);
};

} // namespace Storage
} // namespace Acheron
