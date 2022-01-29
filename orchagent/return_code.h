#pragma once

#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "sai_serialize.h"
#include "status_code_util.h"

extern "C"
{
#include "sai.h"
}

using swss::StatusCode;

// RETURN_IF_ERROR evaluates an expression that returns a ReturnCode. If the
// result is not ok, returns the result. Otherwise, continues.
//
// Example:
//   ReturnCode Foo() {...}
//   ReturnCode Bar() {
//     RETURN_IF_ERROR(Foo());
//     return ReturnCode();
//   }
#define RETURN_IF_ERROR(expr)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        ReturnCode RETURN_IF_ERROR_RC_ = expr;                                                                         \
        if (!RETURN_IF_ERROR_RC_.ok())                                                                                 \
            return RETURN_IF_ERROR_RC_;                                                                                \
    } while (0)

// LOG_ERROR_AND_RETURN evaluates an expression that should returns an error
// ReturnCode. Logs the error message in the ReturnCode by calling
// SWSS_LOG_ERROR and returns.
#define LOG_ERROR_AND_RETURN(expr)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        ReturnCode LOG_ERROR_AND_RETURN_RC_ = expr;                                                                    \
        SWSS_LOG_ERROR("%s", LOG_ERROR_AND_RETURN_RC_.message().c_str());                                              \
        return LOG_ERROR_AND_RETURN_RC_;                                                                               \
    } while (0)

// Same as RETURN_IF_ERROR, plus a call of SWSS_LOG_ERROR for the return code
// error message.
#define LOG_AND_RETURN_IF_ERROR(expr)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ReturnCode LOG_AND_RETURN_IF_ERROR_RC_ = expr;                                                                 \
        if (!LOG_AND_RETURN_IF_ERROR_RC_.ok())                                                                         \
        {                                                                                                              \
            SWSS_LOG_ERROR("%s", LOG_AND_RETURN_IF_ERROR_RC_.message().c_str());                                       \
            return LOG_AND_RETURN_IF_ERROR_RC_;                                                                        \
        }                                                                                                              \
    } while (0)

#define RETURNCODE_MACROS_IMPL_CONCAT_INNER_(x, y) x##y

#define RETURNCODE_MACROS_IMPL_CONCAT_(x, y) RETURNCODE_MACROS_IMPL_CONCAT_INNER_(x, y)

// ASSIGN_OR_RETURN evaluates an expression that returns a ReturnCodeOr. If the
// result is ok, the value is saved to dest. Otherwise, the ReturnCode is
// returned.
//
// Example:
//   ReturnCodeOr<int> Foo() {...}
//   ReturnCode Bar() {
//     ASSIGN_OR_RETURN(int value, Foo());
//     std::cout << "value: " << value;
//     return ReturnCode();
//   }
#define ASSIGN_OR_RETURN(dest, expr)                                                                                   \
    auto RETURNCODE_MACROS_IMPL_CONCAT_(ASSIGN_OR_RETURN_RESULT_, __LINE__) = expr;                                    \
    if (!RETURNCODE_MACROS_IMPL_CONCAT_(ASSIGN_OR_RETURN_RESULT_, __LINE__).ok())                                      \
    {                                                                                                                  \
        return RETURNCODE_MACROS_IMPL_CONCAT_(ASSIGN_OR_RETURN_RESULT_, __LINE__).status();                            \
    }                                                                                                                  \
    dest = std::move(RETURNCODE_MACROS_IMPL_CONCAT_(ASSIGN_OR_RETURN_RESULT_, __LINE__).value())

// CHECK_ERROR_AND_LOG evaluates an expression that returns a sai_status_t. If
// the result is not SAI_STATUS_SUCCESS, it will log an error message.
//
// Example:
// CHECK_ERROR_AND_LOG(
//   sai_router_intfs_api->set_router_interface_attribute(...),
//   "error message" << " stream");
#define CHECK_ERROR_AND_LOG(expr, msg_stream)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        sai_status_t CHECK_ERROR_AND_LOG_SAI_ = expr;                                                                  \
        if (CHECK_ERROR_AND_LOG_SAI_ != SAI_STATUS_SUCCESS)                                                            \
        {                                                                                                              \
            std::stringstream CHECK_ERROR_AND_LOG_SS_;                                                                 \
            CHECK_ERROR_AND_LOG_SS_ << msg_stream;                                                                     \
            SWSS_LOG_ERROR("%s SAI_STATUS: %s", CHECK_ERROR_AND_LOG_SS_.str().c_str(),                                 \
                           sai_serialize_status(CHECK_ERROR_AND_LOG_SAI_).c_str());                                    \
        }                                                                                                              \
    } while (0)

// CHECK_ERROR_AND_LOG_AND_RETURN evaluates an expression that returns a
// sai_status_t. If the result is not SAI_STATUS_SUCCESS, it will log an error
// message and return a ReturnCode.
//
// Example:
// CHECK_ERROR_AND_LOG_AND_RETURN(
//   sai_router_intfs_api->set_router_interface_attribute(...),
//   "error message" << " stream");
#define CHECK_ERROR_AND_LOG_AND_RETURN(expr, msg_stream)                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        sai_status_t CHECK_ERROR_AND_LOG_AND_RETURN_SAI_ = expr;                                                       \
        if (CHECK_ERROR_AND_LOG_AND_RETURN_SAI_ != SAI_STATUS_SUCCESS)                                                 \
        {                                                                                                              \
            ReturnCode CHECK_ERROR_AND_LOG_AND_RETURN_RC_ = ReturnCode(CHECK_ERROR_AND_LOG_AND_RETURN_SAI_)            \
                                                            << msg_stream;                                             \
            SWSS_LOG_ERROR("%s SAI_STATUS: %s", CHECK_ERROR_AND_LOG_AND_RETURN_RC_.message().c_str(),                  \
                           sai_serialize_status(CHECK_ERROR_AND_LOG_AND_RETURN_SAI_).c_str());                         \
            return CHECK_ERROR_AND_LOG_AND_RETURN_RC_;                                                                 \
        }                                                                                                              \
    } while (0)

// This macro raises critical state to indicate that something is seriously
// wrong in the system. Currently, this macro just logs an error message.
// TODO: Implement this macro.
#define SWSS_RAISE_CRITICAL_STATE(err_str)                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        std::string err_msge = err_str;                                                                                \
        SWSS_LOG_ERROR("Orchagent is in critical state: %s", err_msge.c_str());                                        \
    } while (0)

// RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL returns an error status of
// SWSS_RC_INTERNAL. It also logs the error message and reports critical state.
#define RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(msg_stream)                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        ReturnCode RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL_RC_ = ReturnCode(StatusCode::SWSS_RC_INTERNAL)             \
                                                                  << msg_stream;                                       \
        SWSS_LOG_ERROR("%s", RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL_RC_.message().c_str());                          \
        SWSS_RAISE_CRITICAL_STATE(RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL_RC_.message());                             \
        return RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL_RC_;                                                           \
    } while (0)

class ReturnCode
{
  public:
    ReturnCode()
        : status_(StatusCode::SWSS_RC_SUCCESS), stream_(std::ios_base::out | std::ios_base::ate), is_sai_(false)
    {
    }

    ReturnCode(const StatusCode &status, const std::string &message = "")
        : status_(status), stream_(std::ios_base::out | std::ios_base::ate), is_sai_(false)
    {
        stream_ << message;
    }

    ReturnCode(const sai_status_t &status, const std::string &message = "")
        : stream_(std::ios_base::out | std::ios_base::ate), is_sai_(true)
    {
        if (m_saiStatusCodeLookup.find(status) == m_saiStatusCodeLookup.end())
        {
            status_ = StatusCode::SWSS_RC_UNKNOWN;
        }
        else
        {
            status_ = m_saiStatusCodeLookup[status];
        }
        stream_ << message;
    }

    ReturnCode(const ReturnCode &return_code) : stream_(std::ios_base::out | std::ios_base::ate)
    {
        status_ = return_code.status_;
        stream_ << return_code.stream_.str();
        is_sai_ = return_code.is_sai_;
    }

    ReturnCode &operator=(const ReturnCode &return_code)
    {
        status_ = return_code.status_;
        stream_.str(return_code.stream_.str());
        is_sai_ = return_code.is_sai_;
        return *this;
    }

    ~ReturnCode() = default;

    bool ok() const
    {
        return status_ == StatusCode::SWSS_RC_SUCCESS;
    }

    StatusCode code() const
    {
        return status_;
    }

    std::string codeStr() const
    {
        return swss::statusCodeToStr(status_);
    }

    std::string message() const
    {
        if (stream_.str().empty())
        {
            return codeStr();
        }
        return stream_.str();
    }

    ReturnCode &prepend(const std::string &msg)
    {
        const std::string &tmp = stream_.str();
        stream_.str(msg + tmp);
        return *this;
    }

    std::string toString() const
    {
        return codeStr() + ":" + message();
    }

    // Whether the ReturnCode is generated from a SAI status code or not.
    bool isSai() const
    {
        return is_sai_;
    }

    template <typename T> ReturnCode &operator<<(T val)
    {
        stream_ << val;
        return *this;
    }

    bool operator==(const ReturnCode &x) const
    {
        return status_ == x.status_ && message() == x.message();
    }

    bool operator!=(const ReturnCode &x) const
    {
        return status_ != x.status_ || message() != x.message();
    }

    bool operator==(const StatusCode &x) const
    {
        return status_ == x;
    }

    bool operator!=(const StatusCode &x) const
    {
        return status_ != x;
    }

  private:
    // SAI codes that are not included in this lookup map will map to
    // SWSS_RC_UNKNOWN. This includes the general SAI failure: SAI_STATUS_FAILURE.
    std::unordered_map<sai_status_t, StatusCode> m_saiStatusCodeLookup = {
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
    };

    StatusCode status_;
    std::stringstream stream_;
    // Whether the ReturnCode is generated from a SAI status code or not.
    bool is_sai_;
};

inline bool operator==(const StatusCode &lhs, const ReturnCode &rhs)
{
    return lhs == rhs.code();
}

inline bool operator!=(const StatusCode &lhs, const ReturnCode &rhs)
{
    return lhs != rhs.code();
}

template <typename T> class ReturnCodeOr
{
  public:
    using value_type = T;

    // Value Constructors.
    ReturnCodeOr(const T &value) : return_code_(ReturnCode()), value_(std::unique_ptr<T>(new T(value)))
    {
    }
    ReturnCodeOr(T &&value) : return_code_(ReturnCode()), value_(std::unique_ptr<T>(new T(std::move(value))))
    {
    }

    // ReturnCode constructors.
    ReturnCodeOr(const ReturnCode &return_code) : return_code_(return_code)
    {
        assert(!return_code.ok());
    }

    // ReturnCode accessors.
    bool ok() const
    {
        return return_code_.ok();
    }
    const ReturnCode &status() const
    {
        return return_code_;
    }

    // Value accessors.
    const T &value() const &
    {
        assert(return_code_.ok());
        return *value_;
    }
    T &value() &
    {
        assert(return_code_.ok());
        return *value_;
    }
    const T &&value() const &&
    {
        assert(return_code_.ok());
        return std::move(*value_);
    }
    T &&value() &&
    {
        assert(return_code_.ok());
        return std::move(*value_);
    }

    const T &operator*() const &
    {
        return value();
    }
    T &operator*() &
    {
        return value();
    }
    const T &&operator*() const &&
    {
        return value();
    }
    T &&operator*() &&
    {
        return value();
    }

    const T *operator->() const
    {
        return value_.get();
    }
    T *operator->()
    {
        return value_.get();
    }

  private:
    ReturnCode return_code_;
    std::unique_ptr<T> value_;
};
