#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hiredis/hiredis.h>
#include <iostream>

// Add a global redisReply for user to mock
redisReply *mockReply = nullptr;

static redisReply *duplicateRedisReply(const redisReply *src)
{
    if (src == nullptr)
    {
        return nullptr;
    }

    redisReply *dst = (redisReply *)calloc(1, sizeof(redisReply));
    if (dst == nullptr)
    {
        return nullptr;
    }

    dst->type = src->type;
    dst->integer = src->integer;

    if (src->str != nullptr)
    {
        size_t len = src->len;

        if (len == 0)
        {
            len = strlen(src->str);
        }

        dst->len = len;
        dst->str = (char *)calloc(len + 1, sizeof(char));
        if (dst->str != nullptr)
        {
            memcpy(dst->str, src->str, len);
        }
    }
    else
    {
        dst->len = src->len;
    }

    dst->elements = 0;
    if (src->elements > 0 && src->element != nullptr)
    {
        dst->element = (redisReply **)calloc(src->elements, sizeof(redisReply *));
        if (dst->element != nullptr)
        {
            dst->elements = src->elements;
            for (size_t i = 0; i < src->elements; i++)
            {
                dst->element[i] = duplicateRedisReply(src->element[i]);
            }
        }
    }

    return dst;
}

int redisGetReply(redisContext *c, void **reply)
{
    if (mockReply == nullptr)
    {
        *reply = calloc(1, sizeof(redisReply));
        ((redisReply *)*reply)->type = 3;
    }
    else
    {
        // Return a copy so RedisReply can free it; mockReply stays until the test clears it.
        *reply = duplicateRedisReply(mockReply);
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

void redisFree(redisContext *c)
{
    if (c == nullptr)
    {
        return;
    }

    if (c->fd >= 0)
    {
        close(c->fd);
        c->fd = -1;
    }

    if (c->connection_type == REDIS_CONN_TCP)
    {
        free(c->tcp.host);
    }
    else if (c->connection_type == REDIS_CONN_UNIX)
    {
        free(c->unix_sock.path);
    }

    free(c);
}
