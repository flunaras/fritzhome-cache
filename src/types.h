#pragma once
#include <string>
#include <map>

// Shared request/response types used between router, fritzClient, and server.
// Keeping these separate avoids pulling httplib.h into every header.

using Headers = std::map<std::string, std::string>;

struct ProxyRequest {
    std::string method;
    std::string path;             // URL path only (no query string)
    std::string path_and_query;   // full path including '?...' if present
    Headers     headers;
    std::string body;
};

struct ProxyResponse {
    int         status         = 200;
    std::string body;
    Headers     headers;
    bool        upstream_error = false; // true → return 502 to client
};
