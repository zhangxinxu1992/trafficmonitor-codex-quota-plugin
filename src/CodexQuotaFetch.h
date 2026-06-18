#pragma once

#include "CodexQuotaCore.h"

#include <string>

namespace codexquota
{
struct FetchResult
{
    bool success{};
    UsageSnapshot usage;
    std::wstring error;
    int http_status{};
};

std::wstring GetDefaultAuthPath();
FetchResult FetchUsageSnapshot();
}
