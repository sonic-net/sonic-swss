#ifndef SWSS_TESTS_RECORDER_TEST_HELPER_H
#define SWSS_TESTS_RECORDER_TEST_HELPER_H

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

namespace swss_test
{

// Creates a unique temporary directory for recorder unit tests. On scope exit it removes the
// tracked files and the directory itself, so cleanup also runs when a gtest ASSERT_* returns early.
class ScopedRecorderWorkDir
{
public:
    explicit ScopedRecorderWorkDir(const std::string& dirTemplate = "/tmp/swss-recorder-ut-XXXXXX")
    {
        std::vector<char> buffer(dirTemplate.begin(), dirTemplate.end());
        buffer.push_back('\0');
        if (mkdtemp(buffer.data()) != nullptr)
        {
            m_dirname = buffer.data();
        }
    }

    const std::string& dirname() const
    {
        return m_dirname;
    }

    bool created() const
    {
        return !m_dirname.empty();
    }

    void trackFile(const std::string& fullpath)
    {
        m_files.push_back(fullpath);
    }

    ~ScopedRecorderWorkDir()
    {
        for (const auto& path : m_files)
        {
            (void)remove(path.c_str());
        }

        if (!m_dirname.empty())
        {
            (void)rmdir(m_dirname.c_str());
        }
    }

    ScopedRecorderWorkDir(const ScopedRecorderWorkDir&) = delete;
    ScopedRecorderWorkDir& operator=(const ScopedRecorderWorkDir&) = delete;

private:
    std::string m_dirname;
    std::vector<std::string> m_files;
};

} // namespace swss_test

#endif // SWSS_TESTS_RECORDER_TEST_HELPER_H
