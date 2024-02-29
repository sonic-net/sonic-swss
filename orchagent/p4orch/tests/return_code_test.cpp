#include "return_code.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>

extern "C"
{
#include "sai.h"
}

namespace
{

TEST(ReturnCodeTest, SuccessCode)
{
    auto return_code = ReturnCode();
    EXPECT_TRUE(return_code.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code.code());
    EXPECT_EQ("SWSS_RC_SUCCESS", return_code.codeStr());
    EXPECT_EQ("SWSS_RC_SUCCESS", return_code.message());
    EXPECT_EQ("SWSS_RC_SUCCESS:SWSS_RC_SUCCESS", return_code.toString());
    EXPECT_FALSE(return_code.isSai());
}

TEST(ReturnCodeTest, SuccessSaiCode)
{
    auto return_code = ReturnCode(SAI_STATUS_SUCCESS);
    EXPECT_TRUE(return_code.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code);
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code.code());
    EXPECT_EQ("SWSS_RC_SUCCESS", return_code.codeStr());
    EXPECT_EQ("SWSS_RC_SUCCESS", return_code.message());
    EXPECT_EQ("SWSS_RC_SUCCESS:SWSS_RC_SUCCESS", return_code.toString());
    EXPECT_TRUE(return_code.isSai());
}

TEST(ReturnCodeTest, ErrorStatusCode)
{
    auto return_code = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid arguments.";
    EXPECT_FALSE(return_code.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, return_code);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, return_code.code());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM", return_code.codeStr());
    EXPECT_EQ("Invalid arguments.", return_code.message());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM:Invalid arguments.", return_code.toString());
    EXPECT_FALSE(return_code.isSai());
}

TEST(ReturnCodeTest, ErrorSaiCode)
{
    sai_status_t sai_statue = SAI_STATUS_NOT_IMPLEMENTED;
    auto return_code = ReturnCode(sai_statue) << "SAI error: " << sai_statue;
    EXPECT_FALSE(return_code.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, return_code);
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, return_code.code());
    EXPECT_EQ("SWSS_RC_UNIMPLEMENTED", return_code.codeStr());
    EXPECT_EQ("SAI error: -15", return_code.message());
    EXPECT_EQ("SWSS_RC_UNIMPLEMENTED:SAI error: -15", return_code.toString());
    EXPECT_TRUE(return_code.isSai());
}

TEST(ReturnCodeTest, ReturnCodeCopy)
{
    ReturnCode return_code_1 = ReturnCode() << "SUCCESS";
    ReturnCode return_code_2 = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid arguments.";
    return_code_1 = return_code_2;
    EXPECT_FALSE(return_code_1.ok());
    EXPECT_EQ(return_code_1, StatusCode::SWSS_RC_INVALID_PARAM);
    EXPECT_EQ(return_code_1.code(), StatusCode::SWSS_RC_INVALID_PARAM);
    EXPECT_EQ("SWSS_RC_INVALID_PARAM", return_code_1.codeStr());
    EXPECT_EQ("Invalid arguments.", return_code_1.message());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM:Invalid arguments.", return_code_1.toString());
    EXPECT_EQ(return_code_1, return_code_2);
    EXPECT_FALSE(return_code_1 != return_code_2);
}

TEST(ReturnCodeTest, PrependStringInMsg)
{
    auto return_code = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Detailed reasons.";
    return_code.prepend("General statement - ");
    EXPECT_FALSE(return_code.ok());
    EXPECT_FALSE(StatusCode::SWSS_RC_INVALID_PARAM != return_code);
    EXPECT_FALSE(return_code != StatusCode::SWSS_RC_INVALID_PARAM);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, return_code.code());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM", return_code.codeStr());
    EXPECT_EQ("General statement - Detailed reasons.", return_code.message());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM:General statement - Detailed reasons.", return_code.toString());
}

TEST(ReturnCodeTest, CopyAndAppendStringInMsg)
{
    ReturnCode return_code;
    return_code = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Detailed reasons.";
    EXPECT_EQ("Detailed reasons.", return_code.message());
    return_code << " More details.";
    EXPECT_FALSE(return_code.ok());
    EXPECT_FALSE(StatusCode::SWSS_RC_INVALID_PARAM != return_code);
    EXPECT_FALSE(return_code != StatusCode::SWSS_RC_INVALID_PARAM);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, return_code.code());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM", return_code.codeStr());
    EXPECT_EQ("Detailed reasons. More details.", return_code.message());
    EXPECT_EQ("SWSS_RC_INVALID_PARAM:Detailed reasons. More details.", return_code.toString());
}

TEST(ReturnCodeTest, SaiCodeToReturnCodeMapping)
{
    std::unordered_map<sai_status_t, StatusCode> expect_mapping = {
        {SAI_STATUS_SUCCESS, StatusCode::SWSS_RC_SUCCESS},
        {SAI_STATUS_NOT_SUPPORTED, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_NO_MEMORY, StatusCode::SWSS_RC_NO_MEMORY},
        {SAI_STATUS_INSUFFICIENT_RESOURCES, StatusCode::SWSS_RC_FULL},
        {SAI_STATUS_INVALID_PARAMETER, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_ITEM_ALREADY_EXISTS, StatusCode::SWSS_RC_EXISTS},
        {SAI_STATUS_ITEM_NOT_FOUND, StatusCode::SWSS_RC_NOT_FOUND},
        {SAI_STATUS_TABLE_FULL, StatusCode::SWSS_RC_FULL},
        {SAI_STATUS_NOT_IMPLEMENTED, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_OBJECT_IN_USE, StatusCode::SWSS_RC_IN_USE},
        {SAI_STATUS_NOT_EXECUTED, StatusCode::SWSS_RC_NOT_EXECUTED},
        {SAI_STATUS_FAILURE, StatusCode::SWSS_RC_UNKNOWN},
        {SAI_STATUS_INVALID_ATTRIBUTE_0, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_INVALID_ATTRIBUTE_10, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_INVALID_ATTRIBUTE_MAX, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_INVALID_ATTR_VALUE_0, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_INVALID_ATTR_VALUE_10, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_INVALID_ATTR_VALUE_MAX, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_ATTR_NOT_IMPLEMENTED_0, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_ATTR_NOT_IMPLEMENTED_10, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_UNKNOWN_ATTRIBUTE_0, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_UNKNOWN_ATTRIBUTE_10, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX, StatusCode::SWSS_RC_INVALID_PARAM},
        {SAI_STATUS_ATTR_NOT_SUPPORTED_0, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_ATTR_NOT_SUPPORTED_10, StatusCode::SWSS_RC_UNIMPLEMENTED},
        {SAI_STATUS_ATTR_NOT_SUPPORTED_MAX, StatusCode::SWSS_RC_UNIMPLEMENTED},
    };
    for (const auto &it : expect_mapping)
    {
        ReturnCode return_code(it.first);
        EXPECT_EQ(return_code, it.second);
    }
}

TEST(ReturnCodeTest, ReturnCodeOrHasInt)
{
    ReturnCodeOr<int> return_code_or = 42;
    EXPECT_TRUE(return_code_or.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code_or.status());
    EXPECT_EQ(42, return_code_or.value());
    EXPECT_EQ(42, *return_code_or);
}

TEST(ReturnCodeTest, ReturnCodeOrHasCopyableObject)
{
    class TestObj
    {
      public:
        TestObj(int value) : value_(value)
        {
        }
        int GetValue()
        {
            return value_;
        }

      private:
        int value_ = 0;
    };

    ReturnCodeOr<TestObj> return_code_or = TestObj(42);
    EXPECT_TRUE(return_code_or.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code_or.status());
    EXPECT_EQ(42, return_code_or.value().GetValue());
    EXPECT_EQ(42, (*return_code_or).GetValue());
    EXPECT_EQ(42, return_code_or->GetValue());
}

TEST(ReturnCodeTest, ReturnCodeOrHasMoveableObject)
{
    class TestObj
    {
      public:
        TestObj(int value) : value_(value)
        {
        }
        int GetValue()
        {
            return value_;
        }

      private:
        int value_ = 0;
    };

    ReturnCodeOr<std::unique_ptr<TestObj>> return_code_or = std::make_unique<TestObj>(42);
    EXPECT_TRUE(return_code_or.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, return_code_or.status());
    EXPECT_EQ(42, return_code_or.value()->GetValue());
    EXPECT_EQ(42, (*return_code_or)->GetValue());
    EXPECT_EQ(42, return_code_or->get()->GetValue());
    std::unique_ptr<TestObj> test_obj = std::move(*return_code_or);
    EXPECT_EQ(42, test_obj->GetValue());
}

TEST(ReturnCodeTest, ReturnCodeOrHasReturnCode)
{
    ReturnCodeOr<int> return_code_or = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM);
    EXPECT_FALSE(return_code_or.ok());
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, return_code_or.status());
}

} // namespace
