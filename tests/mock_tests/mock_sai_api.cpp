#include "tests/mock_tests/mock_sai_api.h"

std::set<void (*)()> apply_mock_fns;
std::set<void (*)()> remove_mock_fns;

void MockSaiApis()
{
    if (apply_mock_fns.empty())
    {
        EXPECT_TRUE(false) << "No mock application functions found. Did you call DEFINE_SAI_API_MOCK and INIT_SAI_API_MOCK for the necessary SAI object type?";
    }

    for (auto apply_fn : apply_mock_fns)
    {
        (*apply_fn)();
    }
}

void RestoreSaiApis()
{
    for (auto remove_fn : remove_mock_fns)
    {
        (*remove_fn)();
    }
}