#pragma once

#include <functional>
#include <string>

namespace http {

struct Response {
    int status = 0;          // HTTP status code, 0 when the request failed before a response arrived
    std::string body;        // raw response body
    std::string error;       // transport level error description (empty on success)

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Performs a blocking HTTPS request. `url` must be an absolute https:// URL.
// `contentType` is sent when `body` is non-empty; `extraHeaders` are raw
// "Name: value\r\n" lines appended to the request.
Response request(const std::string& method, const std::string& url, const std::string& body = {},
                 const std::string& contentType = "application/json", const std::string& extraHeaders = {});

// Progress callback for downloads: bytes received so far and the total announced by the
// server (0 when unknown). Return false to cancel the transfer.
using DownloadProgress = std::function<bool(unsigned long long received, unsigned long long total)>;

// Downloads `url` (GET, redirects followed) into `filePath`, streaming to disk. On any
// failure the partial file is removed and `error` (or a non-2xx `status`) is set; the
// body of an error response is returned in `body`. Blocking - run it on a worker thread.
Response downloadToFile(const std::string& url, const std::string& filePath, const DownloadProgress& progress = {});

std::string urlEncode(const std::string& value);

// Opens a URL in the user's default browser.
void openInBrowser(const std::string& url);

// Starts a one-shot HTTP listener on 127.0.0.1:`port`, waits (up to
// `timeoutSeconds`) for a single request and returns its query string
// (e.g. "code=abc&state=xyz"). Returns an empty string on timeout/error and
// fills `error`. Blocking - run it on a worker thread.
std::string waitForLoopbackRedirect(int port, int timeoutSeconds, std::string& error);

// Extracts a parameter value from a query string.
std::string queryParam(const std::string& query, const std::string& name);

} // namespace http
