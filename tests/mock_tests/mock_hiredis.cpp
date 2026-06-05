#include <stdlib.h>
#include <unistd.h>
#include <hiredis/hiredis.h>
#include <iostream>

// Add a global redisReply for user to mock
redisReply *mockReply = nullptr;

int redisGetReply(redisContext *c, void **reply)
{
    if (mockReply == nullptr)
    {
        *reply = calloc(1, sizeof(redisReply));
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
