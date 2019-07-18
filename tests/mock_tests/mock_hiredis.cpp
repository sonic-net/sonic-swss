#include <stdlib.h>
#include <hiredis/hiredis.h>

int redisGetReply(redisContext *c, void **reply)
{
    *reply = calloc(sizeof(redisReply), 1);
    ((redisReply *)*reply)->type = 3;
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