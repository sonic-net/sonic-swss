#ifndef MOCK_SAI_API_H
#define MOCK_SAI_API_H
#include "mock_orchagent_main.h"
#include <gmock/gmock.h>

/*
To mock a particular SAI API:
1. At the top of the test CPP file using the mock, call DEFINE_SAI_API_MOCK or DEFINE_SAI_GENERIC_API_MOCK
   for each SAI API you want to mock.
2. At the top of the test CPP file using the mock, call EXTERN_MOCK_FNS.
3. In the SetUp method of the test class, call INIT_SAI_API_MOCK for each SAI API you want to mock.
4. In the SetUp method of the test class, call MockSaiApis.
5. In the TearDown method of the test class, call RestoreSaiApis.
*/

using ::testing::Return;
using ::testing::NiceMock;

#define EXTERN_MOCK_FNS \
    extern std::set<void (*)()> apply_mock_fns; \
    extern std::set<void (*)()> remove_mock_fns;

EXTERN_MOCK_FNS

#define CREATE_PARAMS(sai_object_type) _In_ const sai_##sai_object_type##_entry_t *sai_object_type##_entry, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list
#define REMOVE_PARAMS(sai_object_type) _In_ const sai_##sai_object_type##_entry_t *sai_object_type##_entry
#define CREATE_ARGS(sai_object_type) sai_object_type##_entry, attr_count, attr_list
#define REMOVE_ARGS(sai_object_type) sai_object_type##_entry
#define GENERIC_CREATE_PARAMS(sai_object_type) _Out_ sai_object_id_t *sai_object_type##_id, _In_ sai_object_id_t switch_id, _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list
#define GENERIC_REMOVE_PARAMS(sai_object_type) _In_ sai_object_id_t sai_object_type##_id
#define GENERIC_CREATE_ARGS(sai_object_type) sai_object_type##_id, switch_id, attr_count, attr_list
#define GENERIC_REMOVE_ARGS(sai_object_type) sai_object_type##_id

/*
The macro DEFINE_SAI_API_MOCK will perform the steps to mock the SAI API for the sai_object_type it is called on:
1. Create a pointer to store the original API
2. Create a new SAI_API where we can safely mock without affecting the original API
3. Define a class with mocked methods to create and remove the object type (to be used with gMock)
4. Create a pointer of the above class
5. Define two wrapper functions to create and remove the object type that has the same signature as the original SAI API function
6. Define a method to apply the mock
7. Define a method to remove the mock
*/
#define DEFINE_SAI_API_MOCK(sai_object_type)                                                                                    \
    static sai_##sai_object_type##_api_t *old_sai_##sai_object_type##_api;                                                             \
    static sai_##sai_object_type##_api_t ut_sai_##sai_object_type##_api;                                                               \
    class mock_sai_##sai_object_type##_api_t                                                                                    \
    {                                                                                                                           \
    public:                                                                                                                     \
        mock_sai_##sai_object_type##_api_t()                                                                                    \
        {                                                                                                                       \
            ON_CALL(*this, create_##sai_object_type##_entry)                                                                    \
                .WillByDefault(                                                                                                 \
                    [this](CREATE_PARAMS(sai_object_type)) {                                                                    \
                        return old_sai_##sai_object_type##_api->create_##sai_object_type##_entry(CREATE_ARGS(sai_object_type)); \
                    });                                                                                                         \
            ON_CALL(*this, remove_##sai_object_type##_entry)                                                                    \
                .WillByDefault(                                                                                                 \
                    [this](REMOVE_PARAMS(sai_object_type)) {                                                                    \
                        return old_sai_##sai_object_type##_api->remove_##sai_object_type##_entry(REMOVE_ARGS(sai_object_type)); \
                    });                                                                                                         \
        }                                                                                                                       \
        MOCK_METHOD3(create_##sai_object_type##_entry, sai_status_t(CREATE_PARAMS(sai_object_type)));                           \
        MOCK_METHOD1(remove_##sai_object_type##_entry, sai_status_t(REMOVE_PARAMS(sai_object_type)));                           \
    };                                                                                                                          \
    static mock_sai_##sai_object_type##_api_t *mock_sai_##sai_object_type##_api;                                                       \
    inline sai_status_t mock_create_##sai_object_type##_entry(CREATE_PARAMS(sai_object_type))                                          \
    {                                                                                                                           \
        return mock_sai_##sai_object_type##_api->create_##sai_object_type##_entry(CREATE_ARGS(sai_object_type));                \
    }                                                                                                                           \
    inline sai_status_t mock_remove_##sai_object_type##_entry(REMOVE_PARAMS(sai_object_type))                                          \
    {                                                                                                                           \
        return mock_sai_##sai_object_type##_api->remove_##sai_object_type##_entry(REMOVE_ARGS(sai_object_type));                \
    }                                                                                                                           \
    inline void apply_sai_##sai_object_type##_api_mock()                                                                               \
    {                                                                                                                           \
        mock_sai_##sai_object_type##_api = new NiceMock<mock_sai_##sai_object_type##_api_t>();                                  \
                                                                                                                                \
        old_sai_##sai_object_type##_api = sai_##sai_object_type##_api;                                                          \
        ut_sai_##sai_object_type##_api = *sai_##sai_object_type##_api;                                                          \
        sai_##sai_object_type##_api = &ut_sai_##sai_object_type##_api;                                                          \
                                                                                                                                \
        sai_##sai_object_type##_api->create_##sai_object_type##_entry = mock_create_##sai_object_type##_entry;                  \
        sai_##sai_object_type##_api->remove_##sai_object_type##_entry = mock_remove_##sai_object_type##_entry;                  \
    }                                                                                                                           \
    inline void remove_sai_##sai_object_type##_api_mock()                                                                              \
    {                                                                                                                           \
        sai_##sai_object_type##_api = old_sai_##sai_object_type##_api;                                                          \
        delete mock_sai_##sai_object_type##_api;                                                                                \
    }

#define DEFINE_SAI_GENERIC_API_MOCK(sai_api_name, sai_object_type)                                                           \
    static sai_##sai_api_name##_api_t *old_sai_##sai_api_name##_api;                                                                \
    static sai_##sai_api_name##_api_t ut_sai_##sai_api_name##_api;                                                                  \
    class mock_sai_##sai_api_name##_api_t                                                                                    \
    {                                                                                                                        \
    public:                                                                                                                  \
        mock_sai_##sai_api_name##_api_t()                                                                                    \
        {                                                                                                                    \
            ON_CALL(*this, create_##sai_object_type)                                                                         \
                .WillByDefault(                                                                                              \
                    [this](GENERIC_CREATE_PARAMS(sai_object_type)) {                                                         \
                        return old_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type)); \
                    });                                                                                                      \
            ON_CALL(*this, remove_##sai_object_type)                                                                         \
                .WillByDefault(                                                                                              \
                    [this](GENERIC_REMOVE_PARAMS(sai_object_type)) {                                                         \
                        return old_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type)); \
                    });                                                                                                      \
        }                                                                                                                    \
        MOCK_METHOD4(create_##sai_object_type, sai_status_t(GENERIC_CREATE_PARAMS(sai_object_type)));                        \
        MOCK_METHOD1(remove_##sai_object_type, sai_status_t(GENERIC_REMOVE_PARAMS(sai_object_type)));                        \
    };                                                                                                                       \
    static mock_sai_##sai_api_name##_api_t *mock_sai_##sai_api_name##_api;                                                          \
    inline sai_status_t mock_create_##sai_object_type(GENERIC_CREATE_PARAMS(sai_object_type))                                       \
    {                                                                                                                        \
        return mock_sai_##sai_api_name##_api->create_##sai_object_type(GENERIC_CREATE_ARGS(sai_object_type));                \
    }                                                                                                                        \
    inline sai_status_t mock_remove_##sai_object_type(GENERIC_REMOVE_PARAMS(sai_object_type))                                       \
    {                                                                                                                        \
        return mock_sai_##sai_api_name##_api->remove_##sai_object_type(GENERIC_REMOVE_ARGS(sai_object_type));                \
    }                                                                                                                        \
    inline void apply_sai_##sai_api_name##_api_mock()                                                                               \
    {                                                                                                                        \
        mock_sai_##sai_api_name##_api = new NiceMock<mock_sai_##sai_api_name##_api_t>();                                     \
                                                                                                                             \
        old_sai_##sai_api_name##_api = sai_##sai_api_name##_api;                                                             \
        ut_sai_##sai_api_name##_api = *sai_##sai_api_name##_api;                                                             \
        sai_##sai_api_name##_api = &ut_sai_##sai_api_name##_api;                                                             \
                                                                                                                             \
        sai_##sai_api_name##_api->create_##sai_object_type = mock_create_##sai_object_type;                                  \
        sai_##sai_api_name##_api->remove_##sai_object_type = mock_remove_##sai_object_type;                                  \
    }                                                                                                                        \
    inline void remove_sai_##sai_api_name##_api_mock()                                                                              \
    {                                                                                                                        \
        sai_##sai_api_name##_api = old_sai_##sai_api_name##_api;                                                             \
        delete mock_sai_##sai_api_name##_api;                                                                                \
    }

// Stores pointers to mock apply/remove functions to avoid needing to manually call each function
#define INIT_SAI_API_MOCK(sai_object_type)                          \
    apply_mock_fns.insert(&apply_sai_##sai_object_type##_api_mock); \
    remove_mock_fns.insert(&remove_sai_##sai_object_type##_api_mock);

void MockSaiApis();
void RestoreSaiApis();
#endif