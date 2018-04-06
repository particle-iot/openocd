/* ANSI-C code produced by gperf version 3.1 */
/* Command-line: gperf -L ANSI-C -t rtos.gperf  */
/* Computed positions: -k'' */

#line 2 "rtos.gperf"
#include "rtos_gperf.h"
#line 8 "rtos.gperf"
#define TOTAL_KEYWORDS 1
#define MIN_WORD_LENGTH 8
#define MAX_WORD_LENGTH 8
#define MIN_HASH_VALUE 0
#define MAX_HASH_VALUE 0
/* maximum key range = 1, duplicates = 0 */

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
/*ARGSUSED*/
static unsigned int
hash (register const char *str, register size_t len)
{
  return 0;
}

struct rtos_lookup_entry *
rtos_in_word_set (register const char *str, register size_t len)
{
  static struct rtos_lookup_entry wordlist[] =
    {
#line 10 "rtos.gperf"
      {"freertos", rtos_freertos}
    };

  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      register unsigned int key = hash (str, len);

      if (key <= MAX_HASH_VALUE)
        {
          register const char *s = wordlist[key].name;

          if (*str == *s && !strcmp (str + 1, s + 1))
            return &wordlist[key];
        }
    }
  return 0;
}
