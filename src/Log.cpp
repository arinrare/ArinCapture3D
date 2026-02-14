#include "Log.h"
#include <fstream>
#include <iostream>
#include <ctime>

#include <windows.h>

namespace Log {
    static std::string LogFile() {
        wchar_t exePath[MAX_PATH] = {0};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return "ArinCapture.log";
        }

        // Strip to directory.
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (!lastSlash) lastSlash = wcsrchr(exePath, L'/');
        if (!lastSlash) {
            return "ArinCapture.log";
        }
        *(lastSlash + 1) = L'\0';

        std::wstring full = std::wstring(exePath) + L"ArinCapture.log";
        int needed = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return "ArinCapture.log";
        }
        std::string out;
        out.resize((size_t)needed - 1);
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    void ToFile(const std::string& msg) {
        static bool firstWrite = true;
        std::ofstream ofs;
        if (firstWrite) {
            ofs.open(LogFile(), std::ios::trunc); // Overwrite on first write
            firstWrite = false;
        } else {
            ofs.open(LogFile(), std::ios::app); // Append after first write
        }
        if (ofs) {
            std::time_t t = std::time(nullptr);
            ofs << std::ctime(&t) << msg << std::endl;
        }
    }

    void Info(const std::string& msg) {
        std::cout << "[INFO] " << msg << std::endl;
        ToFile("[INFO] " + msg);
    }

    void Error(const std::string& msg) {
        std::cerr << "[ERROR] " << msg << std::endl;
        ToFile("[ERROR] " + msg);
    }
}
