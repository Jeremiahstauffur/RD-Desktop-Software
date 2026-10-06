#include "Http.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstdio>
#include <vector>

namespace http {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::string lastErrorText(const char* where) {
    DWORD code = GetLastError();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (WinHTTP error %lu)", where, (unsigned long)code);
    return buf;
}

struct HandleGuard {
    HINTERNET h = nullptr;
    ~HandleGuard() { if (h) WinHttpCloseHandle(h); }
};

} // namespace

std::string urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

Response request(const std::string& method, const std::string& url, const std::string& body,
                 const std::string& contentType, const std::string& extraHeaders) {
    Response res;

    std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = (DWORD)std::size(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = (DWORD)std::size(path);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &parts)) {
        res.error = "Invalid URL";
        return res;
    }

    HandleGuard session;
    session.h = WinHttpOpen(L"RD Desktop Software/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        res.error = lastErrorText("WinHttpOpen");
        return res;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 60000);

    HandleGuard connection;
    connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connection.h) {
        res.error = lastErrorText("WinHttpConnect");
        return res;
    }

    DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HandleGuard req;
    req.h = WinHttpOpenRequest(connection.h, widen(method).c_str(), path, nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.h) {
        res.error = lastErrorText("WinHttpOpenRequest");
        return res;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!body.empty()) headers += L"Content-Type: " + widen(contentType) + L"\r\n";
    if (!extraHeaders.empty()) headers += widen(extraHeaders);

    if (!WinHttpSendRequest(req.h, headers.c_str(), (DWORD)-1L,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                            (DWORD)body.size(), (DWORD)body.size(), 0)) {
        res.error = lastErrorText("WinHttpSendRequest");
        return res;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        res.error = lastErrorText("WinHttpReceiveResponse");
        return res;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)status;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available)) {
            res.error = lastErrorText("WinHttpQueryDataAvailable");
            return res;
        }
        if (available == 0) break;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk.data(), available, &read)) {
            res.error = lastErrorText("WinHttpReadData");
            return res;
        }
        res.body.append(chunk.data(), read);
    }
    return res;
}

void openInBrowser(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string queryParam(const std::string& query, const std::string& name) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        std::string key = pair.substr(0, eq);
        if (key == name) {
            std::string value = eq == std::string::npos ? "" : pair.substr(eq + 1);
            // percent-decode
            std::string out;
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '%' && i + 2 < value.size()) {
                    out.push_back((char)std::strtol(value.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else if (value[i] == '+') {
                    out.push_back(' ');
                } else {
                    out.push_back(value[i]);
                }
            }
            return out;
        }
        pos = amp + 1;
    }
    return "";
}

std::string waitForLoopbackRedirect(int port, int timeoutSeconds, std::string& error) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "Winsock initialisation failed";
        return "";
    }
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = "Could not create listening socket";
        return "";
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        error = "Could not listen on 127.0.0.1:" + std::to_string(port) + " (port in use?)";
        return "";
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval tv{timeoutSeconds, 0};
    int ready = select(0, &set, nullptr, nullptr, &tv);
    if (ready <= 0) {
        closesocket(listener);
        error = "Timed out waiting for the browser to return to the app";
        return "";
    }
    SOCKET client = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (client == INVALID_SOCKET) {
        error = "Failed to accept the browser connection";
        return "";
    }

    char buf[8192];
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    std::string requestLine = n > 0 ? std::string(buf, n) : "";
    std::string query;
    size_t q = requestLine.find('?');
    size_t sp = requestLine.find(' ', q == std::string::npos ? 0 : q);
    if (q != std::string::npos && sp != std::string::npos && sp > q) query = requestLine.substr(q + 1, sp - q - 1);

    std::string html =
        "<!doctype html><html><head><meta charset='utf-8'><title>RepairDesk Desktop</title>"
        "<style>body{font-family:'Segoe UI',sans-serif;background:#202020;color:#fff;display:flex;"
        "align-items:center;justify-content:center;height:100vh;margin:0}div{text-align:center}"
        "h1{font-weight:600}p{opacity:.7}</style></head><body><div><h1>You're connected</h1>"
        "<p>You can close this tab and return to RepairDesk Desktop.</p></div></body></html>";
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                           std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
    send(client, response.c_str(), (int)response.size(), 0);
    shutdown(client, SD_SEND);
    closesocket(client);

    if (query.empty()) error = "The browser did not send an authorization response";
    return query;
}

} // namespace http
