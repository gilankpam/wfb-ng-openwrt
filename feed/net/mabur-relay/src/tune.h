#ifndef TUNE_H
#define TUNE_H
#include <stdint.h>
#include <sys/types.h>

int tune_valid(uint8_t channel, uint8_t sec);          /* 1 if 36..177 and sec<=2 */
const char *tune_sec_str(uint8_t sec);                 /* "HT20" / "HT40+" / "HT40-" / NULL */
int tune_sec_code(const char *s, uint8_t *sec);        /* inverse; 0 ok, -1 unknown */
/* Fills argv = {"iw","dev",mon,"set","channel",chbuf,secstr,NULL}; 0 ok, -1 invalid. */
int tune_build_argv(const char *mon, uint8_t channel, uint8_t sec, char chbuf[4], const char *argv[8]);
/* Parses `iw dev <mon> info` text: the "channel N (F MHz), width: W MHz, center1: C MHz"
 * line. 0 ok (sec from width/center1 vs F), -1 no usable line. */
int tune_parse_info(const char *text, uint8_t *channel, uint8_t *sec);
pid_t tune_spawn(const char *mon, uint8_t channel, uint8_t sec);   /* fork+execvp "iw"; -1 on error */
int tune_readback(const char *mon, uint8_t *channel, uint8_t *sec); /* popen("iw dev <mon> info") */
#endif
