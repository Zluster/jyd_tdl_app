/* inih -- simple .INI file parser */

#ifndef INI_H
#define INI_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef INI_USE_STACK
#define INI_USE_STACK 1
#endif

#ifndef INI_MAX_LINE
#define INI_MAX_LINE 200
#endif

#ifndef INI_INITIAL_ALLOC
#define INI_INITIAL_ALLOC 200
#endif

#ifndef INI_ALLOW_REALLOC
#define INI_ALLOW_REALLOC 0
#endif

#ifndef INI_ALLOW_BOM
#define INI_ALLOW_BOM 1
#endif

#ifndef INI_ALLOW_MULTILINE
#define INI_ALLOW_MULTILINE 1
#endif

#ifndef INI_ALLOW_INLINE_COMMENTS
#define INI_ALLOW_INLINE_COMMENTS 1
#endif

#ifndef INI_INLINE_COMMENT_PREFIXES
#define INI_INLINE_COMMENT_PREFIXES ";"
#endif

#ifndef INI_START_COMMENT_PREFIXES
#define INI_START_COMMENT_PREFIXES ";#"
#endif

#ifndef INI_ALLOW_NO_VALUE
#define INI_ALLOW_NO_VALUE 0
#endif

#ifndef INI_STOP_ON_FIRST_ERROR
#define INI_STOP_ON_FIRST_ERROR 0
#endif

#ifndef INI_HANDLER_LINENO
#define INI_HANDLER_LINENO 0
#endif

#ifndef INI_CALL_HANDLER_ON_NEW_SECTION
#define INI_CALL_HANDLER_ON_NEW_SECTION 0
#endif

typedef int (*ini_handler)(void *user, const char *section, const char *name,
                           const char *value);
typedef int (*ini_reader)(char *str, int num, void *stream);

int ini_parse(const char *filename, ini_handler handler, void *user);
int ini_parse_file(FILE *file, ini_handler handler, void *user);
int ini_parse_stream(ini_reader reader, void *stream, ini_handler handler,
                     void *user);
int ini_parse_string(const char *string, ini_handler handler, void *user);

#ifdef __cplusplus
}
#endif

#endif
