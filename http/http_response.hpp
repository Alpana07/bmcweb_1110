#pragma once
#include "logging.hpp"
#include "nlohmann/json.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http/basic_dynamic_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <utils/hex_utils.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace crow
{

template <typename Adaptor, typename Handler>
class Connection;

struct Response
{
    template <typename Adaptor, typename Handler>
    friend class crow::Connection;
    using response_type =
        boost::beast::http::response<boost::beast::http::string_body>;

    std::optional<response_type> stringResponse;

    nlohmann::json jsonValue;

    void addHeader(const std::string_view key, const std::string_view value)
    {
        stringResponse->set(key, value);
    }

    void addHeader(boost::beast::http::field key, std::string_view value)
    {
        stringResponse->set(key, value);
    }

    Response() : stringResponse(response_type{}) {}

    Response(Response&& res) noexcept :
        stringResponse(std::move(res.stringResponse)), completed(res.completed)
    {
        jsonValue = std::move(res.jsonValue);
        // See note in operator= move handler for why this is needed.
        if (!res.completed)
        {
            completeRequestHandler = std::move(res.completeRequestHandler);
            res.completeRequestHandler = nullptr;
        }
        isAliveHelper = res.isAliveHelper;
        res.isAliveHelper = nullptr;
    }

    ~Response() = default;

    Response(const Response&) = delete;

    Response& operator=(const Response& r) = delete;

    Response& operator=(Response&& r) noexcept
    {
        BMCWEB_LOG_DEBUG << "Moving response containers; this: " << this
                         << "; other: " << &r;
        if (this == &r)
        {
            return *this;
        }
        stringResponse = std::move(r.stringResponse);
        r.stringResponse.emplace(response_type{});
        jsonValue = std::move(r.jsonValue);

        // Only need to move completion handler if not already completed
        // Note, there are cases where we might move out of a Response object
        // while in a completion handler for that response object.  This check
        // is intended to prevent destructing the functor we are currently
        // executing from in that case.
        if (!r.completed)
        {
            completeRequestHandler = std::move(r.completeRequestHandler);
            r.completeRequestHandler = nullptr;
        }
        else
        {
            completeRequestHandler = nullptr;
        }
        completed = r.completed;
        isAliveHelper = std::move(r.isAliveHelper);
        r.isAliveHelper = nullptr;
        return *this;
    }

    void result(unsigned v)
    {
        stringResponse->result(v);
    }

    void result(boost::beast::http::status v)
    {
        stringResponse->result(v);
    }

    boost::beast::http::status result() const
    {
        return stringResponse->result();
    }

    unsigned resultInt() const
    {
        return stringResponse->result_int();
    }

    std::string_view reason() const
    {
        return stringResponse->reason();
    }

    bool isCompleted() const noexcept
    {
        return completed;
    }

    std::string& body()
    {
        return stringResponse->body();
    }

    std::string_view getHeaderValue(std::string_view key) const
    {
        return stringResponse->base()[key];
    }

    void keepAlive(bool k)
    {
        stringResponse->keep_alive(k);
    }

    bool keepAlive() const
    {
        return stringResponse->keep_alive();
    }

    void preparePayload()
    {
        stringResponse->prepare_payload();
    }

    void clear()
    {
        BMCWEB_LOG_DEBUG << this << " Clearing response containers";
        stringResponse.emplace(response_type{});
        jsonValue.clear();
        completed = false;
        expectedHash = std::nullopt;
    }

    void write(std::string_view bodyPart)
    {
        stringResponse->body() += std::string(bodyPart);
    }

    std::string computeEtag() const
    {
        // Only set etag if this request succeeded
        if (result() != boost::beast::http::status::ok)
        {
            return "";
        }
        // and the json response isn't empty
        if (jsonValue.empty())
        {
            return "";
        }
        size_t hashval = std::hash<nlohmann::json>{}(jsonValue);
        return "\"" + intToHexString(hashval, 8) + "\"";
    }

    void end()
    {
        std::string etag = computeEtag();
        if (!etag.empty())
        {
            addHeader(boost::beast::http::field::etag, etag);
        }
        if (completed)
        {
            BMCWEB_LOG_ERROR << this << " Response was ended twice";
            return;
        }
        completed = true;
        BMCWEB_LOG_DEBUG << this << " calling completion handler";
        if (completeRequestHandler)
        {
            BMCWEB_LOG_DEBUG << this << " completion handler was valid";
            completeRequestHandler(*this);
        }
    }

    bool isAlive() const
    {
        return isAliveHelper && isAliveHelper();
    }

    void setCompleteRequestHandler(std::function<void(Response&)>&& handler)
    {
        BMCWEB_LOG_DEBUG << this << " setting completion handler";
        completeRequestHandler = std::move(handler);

        // Now that we have a new completion handler attached, we're no longer
        // complete
        completed = false;
    }

    std::function<void(Response&)> releaseCompleteRequestHandler()
    {
        BMCWEB_LOG_DEBUG << this << " releasing completion handler"
                         << static_cast<bool>(completeRequestHandler);
        std::function<void(Response&)> ret = completeRequestHandler;
        completeRequestHandler = nullptr;
        completed = true;
        return ret;
    }

    void setIsAliveHelper(std::function<bool()>&& handler)
    {
        isAliveHelper = std::move(handler);
    }

    std::function<bool()> releaseIsAliveHelper()
    {
        std::function<bool()> ret = std::move(isAliveHelper);
        isAliveHelper = nullptr;
        return ret;
    }

    void setHashAndHandleNotModified()
    {
        // Can only hash if we have content that's valid
        if (jsonValue.empty() || result() != boost::beast::http::status::ok)
        {
            return;
        }
        size_t hashval = std::hash<nlohmann::json>{}(jsonValue);
        std::string hexVal = "\"" + intToHexString(hashval, 8) + "\"";
        addHeader(boost::beast::http::field::etag, hexVal);
        if (expectedHash && hexVal == *expectedHash)
        {
            jsonValue.clear();
            result(boost::beast::http::status::not_modified);
        }
    }

    void setExpectedHash(std::string_view hash)
    {
        expectedHash = hash;
    }

  private:
    std::optional<std::string> expectedHash;
    bool completed = false;
    std::function<void(Response&)> completeRequestHandler;
    std::function<bool()> isAliveHelper;
};

struct DynamicResponse
{
    using response_type = boost::beast::http::response<
        boost::beast::http::basic_dynamic_body<boost::beast::flat_static_buffer<
            static_cast<std::size_t>(1024 * 1024)>>>;

    std::optional<response_type> bufferResponse;

    void addHeader(const std::string_view key, const std::string_view value)
    {
        bufferResponse->set(key, value);
    }

    void addHeader(boost::beast::http::field key, std::string_view value)
    {
        bufferResponse->set(key, value);
    }

    DynamicResponse() : bufferResponse(response_type{}) {}

    ~DynamicResponse() = default;

    DynamicResponse(const DynamicResponse&) = delete;

    DynamicResponse(DynamicResponse&&) = delete;

    DynamicResponse& operator=(const DynamicResponse& r) = delete;

    DynamicResponse& operator=(DynamicResponse&& r) noexcept
    {
        BMCWEB_LOG_DEBUG << "Moving response containers";
        bufferResponse = std::move(r.bufferResponse);
        r.bufferResponse.emplace(response_type{});
        completed = r.completed;
        return *this;
    }

    void result(boost::beast::http::status v)
    {
        bufferResponse->result(v);
    }

    boost::beast::http::status result()
    {
        return bufferResponse->result();
    }

    unsigned resultInt()
    {
        return bufferResponse->result_int();
    }

    std::string_view reason()
    {
        return bufferResponse->reason();
    }

    bool isCompleted() const noexcept
    {
        return completed;
    }

    void keepAlive(bool k)
    {
        bufferResponse->keep_alive(k);
    }

    bool keepAlive()
    {
        return bufferResponse->keep_alive();
    }

    void preparePayload()
    {
        bufferResponse->prepare_payload();
    }

    void clear()
    {
        BMCWEB_LOG_DEBUG << this << " Clearing response containers";
        bufferResponse.emplace(response_type{});
        completed = false;
    }

    void end()
    {
        if (completed)
        {
            BMCWEB_LOG_DEBUG << "Dynamic response was ended twice";
            return;
        }
        completed = true;
        BMCWEB_LOG_DEBUG << "calling completion handler";
        if (completeRequestHandler)
        {
            BMCWEB_LOG_DEBUG << "completion handler was valid";
            completeRequestHandler();
        }
    }

    bool isAlive()
    {
        return isAliveHelper && isAliveHelper();
    }
    std::function<void()> completeRequestHandler;

  private:
    bool completed{};
    std::function<bool()> isAliveHelper;
};

} // namespace crow
