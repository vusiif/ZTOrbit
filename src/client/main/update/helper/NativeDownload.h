#pragma once
#include <string>
#include <vector>

// Native downloader mode of z2_updater.exe. The host passes an already verified
// artifact; the helper streams /file to disk and independently verifies it.
int runNativeDownload(const std::vector<std::string> &arguments);
