#pragma once
#include <string>

namespace Log {
    void Info(const std::string& msg);
    void Error(const std::string& msg);
    void ToFile(const std::string& msg);
}
