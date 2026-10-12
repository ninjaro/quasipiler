#ifndef TEST_SOURCE_HPP
#define TEST_SOURCE_HPP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

/// A private temporary directory: no fixed filenames shared between test runs.
class test_source_file final {
public:
    explicit test_source_file(const std::string& text) {
        const auto stamp
            = std::chrono::steady_clock::now().time_since_epoch().count();
        for (size_t attempt = 0;; ++attempt) {
            directory_ = std::filesystem::temp_directory_path()
                / ("quasipiler-test-" + std::to_string(stamp) + "-"
                   + std::to_string(attempt));
            if (std::filesystem::create_directory(directory_))
                break;
        }
        path = directory_ / "input.qc";
        write(text);
    }

    ~test_source_file() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    void write(const std::string& text) const {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << text;
        file.close();
    }

    test_source_file(const test_source_file&) = delete;
    test_source_file& operator=(const test_source_file&) = delete;
    std::filesystem::path path;

private:
    std::filesystem::path directory_;
};

#endif // TEST_SOURCE_HPP
