#ifndef ARBITER_IS_AMALGAMATION
#include <arbiter/arbiter.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

#ifndef ARBITER_IS_AMALGAMATION
#include <arbiter/arbiter.hpp>
#include <arbiter/drivers/fs.hpp>
#include <arbiter/drivers/gdrive.hpp>
#include <arbiter/third/xml/xml.hpp>
#include <arbiter/util/crypto.hpp>
#include <arbiter/third/json/json.hpp>
#endif

namespace arbiter
{

namespace
{
    const std::string baseGetUrl("https://www.googleapis.com/drive/v3/files/");

    // We still need to use API V1 for GET requests since V2 is poorly
    // documented and doesn't correctly support the Range header.  Hopefully
    // we can switch to V2 at some point.
    const bool legacy(true);

    const std::string listUrl("https://www.googleapis.com/drive/v3/files");
    const std::string continueListUrl(listUrl + "/continue");

    const auto ins([](unsigned char lhs, unsigned char rhs)
    {
        return std::tolower(lhs) == std::tolower(rhs);
    });

    std::string toSanitizedString(const Json::Value& v)
    {
        Json::FastWriter writer;
        std::string f(writer.write(v));
        f.erase(std::remove(f.begin(), f.end(), '\n'), f.end());
        return f;
    }

    const std::string dirTag("folder");
    const std::string fileTag("file");
}

namespace drivers
{

GDrive::GDrive(HttpPool& pool, const GDriveAuth auth)
    : m_pool(pool)
    , m_auth(auth)
{ }

std::unique_ptr<GDrive> GDrive::create(
        HttpPool& pool,
        const Json::Value& json)
{
    std::unique_ptr<GDrive> gdrive;

    auto check_member= [json](const std::string& member)
    {
        if (!json.isMember(member))
        {
            std::ostringstream oss;
            oss << "member '" << member << "' is not present in config";
            throw ArbiterError(oss.str());
        }
    };

    check_member("token");
    check_member("app");

    gdrive.reset(new GDrive(pool, GDriveAuth(json["token"].asString(),
                                             json["app"].asString())));

    return gdrive;
}

Headers GDrive::httpGetHeaders() const
{
    Headers headers;

    headers["Authorization"] = "Bearer " + m_auth.token();

    headers["Transfer-Encoding"] = "";
    headers["Expect"] = "";

    return headers;
}

Headers GDrive::httpPostHeaders() const
{
    Headers headers;

    headers["Authorization"] = "Bearer " + m_auth.token();
    headers["Transfer-Encoding"] = "chunked";
    headers["Expect"] = "100-continue";
    headers["Content-Type"] = "application/json";

    return headers;
}

bool GDrive::get(const std::string rawPath, std::vector<char>& data) const
{
    return buildRequestAndGet(rawPath, data);
}

bool GDrive::buildRequestAndGet(
        const std::string rawPath,
        std::vector<char>& data,
        const Headers userHeaders) const
{
    const std::string path(Http::sanitize(rawPath));

    Headers headers(httpGetHeaders());

    Json::Value json;
    json["path"] = std::string("/" + path);

    headers.insert(userHeaders.begin(), userHeaders.end());

    auto http(m_pool.acquire());

    HttpResponse res(http.get(baseGetUrl, headers));

    if (res.ok())
    {
        if (
                (legacy && !res.headers().count("Content-Length")) ||
                (!legacy && !res.headers().count("original-content-length")))
        {
            return false;
        }

        const std::size_t size(
                std::stol( res.headers().at("original-content-length")));

        data = res.data();

        if (size == res.data().size())
        {
            return true;
        }
        else
        {
            std::ostringstream oss;
            oss << "Data size check failed. State size was '" << size
                << "' and downloaded size was '" << res.data().size() << "'";
            throw ArbiterError(oss.str());
        }
    }
    else
    {
        std::string message(res.data().data(), res.data().size());
        throw ArbiterError(
                "Server response: " + std::to_string(res.code()) + " - '" +
                message + "'");
    }

    return false;
}

void GDrive::put(std::string rawPath, const std::vector<char>& data) const
{
    throw ArbiterError("PUT not yet supported for " + type());
}

std::string GDrive::continueFileInfo(std::string cursor) const
{
    Headers headers(httpPostHeaders());

    auto http(m_pool.acquire());

    Json::Value json;
    json["nextPageToken"] = cursor;
    std::string f = toSanitizedString(json);

    std::vector<char> postData(f.begin(), f.end());
    HttpResponse res(http.post(continueListUrl, postData, headers));

    if (res.ok())
    {
        return std::string(res.data().data(), res.data().size());
    }
    else
    {
        std::string message(res.data().data(), res.data().size());
        throw ArbiterError(
                "Server response: " + std::to_string(res.code()) + " - '" +
                message + "'");
    }

    return std::string("");
}

std::vector<std::string> GDrive::glob(std::string rawPath, bool verbose) const
{
    std::vector<std::string> results;

    const std::string path(
            Http::sanitize(rawPath.substr(0, rawPath.size() - 2)));
    std::cout << "sanitized: " << path << std::endl;

    auto listPath = [this](std::string path)->std::string
    {
        auto http(m_pool.acquire());
        Headers headers(httpGetHeaders());

        std::string url = listUrl + "?key=" +  m_auth.app();
        std::cout << "listUrl: " << url << std::endl;
        HttpResponse res(http.get(url, headers));

        if (res.ok())
        {
            return std::string(res.data().data(), res.data().size());
        }
        else if (res.code() == 409)
        {
            return "";
        }
        else
        {
            std::string message(res.data().data(), res.data().size());
            throw ArbiterError(
                    "Server response: " + std::to_string(res.code()) + " - '" +
                    message + "'");
        }
    };

    bool more(false);
    std::string cursor("");

    auto processPath = [verbose, &results, &more, &cursor](std::string data)
    {
        if (data.empty()) return;

        if (verbose) std::cout << '.';

        Json::Value json;
        Json::Reader reader;
        reader.parse(data, json, false);

        const Json::Value& entries(json["entries"]);

        if (entries.isNull())
        {
            throw ArbiterError("Returned JSON from GDrive was NULL");
        }
        if (!entries.isArray())
        {
            throw ArbiterError("Returned JSON from GDrive was not an array");
        }

        more = json["has_more"].asBool();
        cursor = json["nextPageToken"].asString();

        for (std::size_t i(0); i < entries.size(); ++i)
        {
            const Json::Value& v(entries[static_cast<Json::ArrayIndex>(i)]);
            const std::string tag(v[".tag"].asString());

            // Only insert files.
            if (std::equal(tag.begin(), tag.end(), fileTag.begin(), ins))
            {
                // Results already begin with a slash.
                results.push_back("gdrive:/" + v["path_lower"].asString());
            }
        }
    };

    processPath(listPath(path));

    if (more)
    {
        do
        {
            processPath(continueFileInfo(cursor));
        }
        while (more);
    }

    return results;
}



// These functions allow a caller to directly pass additional headers into
// their GET request.  This is only applicable when using the GDrive driver
// directly, as these are not available through the Arbiter.

std::vector<char> GDrive::getBinary(std::string rawPath, Headers headers) const
{
    std::vector<char> data;
    const std::string stripped(Arbiter::stripType(rawPath));
    if (!buildRequestAndGet(stripped, data, headers))
    {
        throw ArbiterError("Couldn't GDrive GET " + rawPath);
    }

    return data;
}

std::string GDrive::get(std::string rawPath, Headers headers) const
{
    std::vector<char> data(getBinary(rawPath, headers));
    return std::string(data.begin(), data.end());
}

} // namespace drivers
} // namespace arbiter

