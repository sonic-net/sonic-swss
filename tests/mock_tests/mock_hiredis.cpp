#include <stdlib.h>
#include <hiredis/hiredis.h>
#include <iostream>

// Add a global redisReply for user to mock
redisReply *mockReply = nullptr;

int redisGetReply(redisContext *c, void **reply)
{
    if (mockReply == nullptr)
    {
        *reply = calloc(sizeof(redisReply), 1);
        ((redisReply *)*reply)->type = 3;
    }
    else
    {
        *reply = mockReply;
    }
    return 0;
}

int redisAppendFormattedCommand(redisContext *c, const char *cmd, size_t len)
{
    return 0;
}

int redisvAppendCommand(redisContext *c, const char *format, va_list ap)
{
    return 0;
}

int redisAppendCommand(redisContext *c, const char *format, ...)
{
    return 0;
}

int redisGetReplyFromReader(redisContext *c, void **reply)
{
    return 0;
}
