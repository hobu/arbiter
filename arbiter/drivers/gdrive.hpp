#pragma once

#include <memory>
#include <string>
#include <vector>

#ifndef ARBITER_IS_AMALGAMATION
#include <arbiter/driver.hpp>
#include <arbiter/drivers/http.hpp>
#endif

namespace arbiter
{

namespace drivers
{

/** @brief %GDrive authentication information. */
class GDriveAuth
{
public:
    explicit GDriveAuth(std::string token, std::string app) : m_token(token), m_app(app) { }
    std::string token() const { return m_token; }
    std::string app() const { return m_app; }

private:
    std::string m_token;
    std::string m_app;
};

/** @brief %GDrive driver. */
class GDrive : public CustomHeaderDriver
{
public:
    GDrive(HttpPool& pool, GDriveAuth auth);

    /** Try to construct a %GDrive Driver.  Searches @p json for the key
     * `token` to construct a GDriveAuth.
     */
    static std::unique_ptr<GDrive> create(
            HttpPool& pool,
            const Json::Value& json);

    virtual std::string type() const override { return "gdrive"; }
    virtual void put(
            std::string path,
            const std::vector<char>& data) const override;

    /** Inherited from CustomHeaderDriver. */
    virtual std::string get(std::string path, Headers headers) const override;

    /** Inherited from CustomHeaderDriver. */
    virtual std::vector<char> getBinary(
            std::string path,
            Headers headers) const override;

private:
    virtual bool get(std::string path, std::vector<char>& data) const override;
    virtual std::vector<std::string> glob(
            std::string path,
            bool verbose) const override;

    bool buildRequestAndGet(
            std::string path,
            std::vector<char>& data,
            Headers headers = Headers()) const;

    std::string continueFileInfo(std::string cursor) const;

    Headers httpGetHeaders() const;
    Headers httpPostHeaders() const;

    HttpPool& m_pool;
    GDriveAuth m_auth;
};

} // namespace drivers
} // namespace arbiter

