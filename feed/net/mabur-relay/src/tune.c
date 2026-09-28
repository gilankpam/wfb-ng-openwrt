#include "tune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int tune_valid(uint8_t channel, uint8_t sec) { return channel >= 36 && channel <= 177 && sec <= 2; }

const char *tune_sec_str(uint8_t sec) {
  static const char *s[] = {"HT20", "HT40+", "HT40-"};
  return sec <= 2 ? s[sec] : NULL;
}

int tune_sec_code(const char *s, uint8_t *sec) {
  for (uint8_t i = 0; i <= 2; i++)
    if (strcmp(s, tune_sec_str(i)) == 0) { *sec = i; return 0; }
  return -1;
}

int tune_build_argv(const char *mon, uint8_t channel, uint8_t sec, char chbuf[4], const char *argv[8]) {
  if (!tune_valid(channel, sec)) return -1;
  snprintf(chbuf, 4, "%u", (unsigned)channel);
  argv[0] = "iw"; argv[1] = "dev"; argv[2] = mon; argv[3] = "set"; argv[4] = "channel";
  argv[5] = chbuf; argv[6] = tune_sec_str(sec); argv[7] = NULL;
  return 0;
}

int tune_parse_info(const char *text, uint8_t *channel, uint8_t *sec) {
  const char *p = text;
  while ((p = strstr(p, "channel ")) != NULL) {
    unsigned ch, freq, width, c1;
    const char *w = strstr(p, "width: "), *c = strstr(p, "center1: ");
    const char *eol = strchr(p, '\n');
    if (sscanf(p, "channel %u (%u MHz)", &ch, &freq) == 2 && w && c && (!eol || (w < eol && c < eol)) &&
        sscanf(w, "width: %u", &width) == 1 && sscanf(c, "center1: %u", &c1) == 1) {
      if (ch > 255) return -1;
      if (width == 20) { *channel = (uint8_t)ch; *sec = 0; return 0; }
      if (width == 40) { *channel = (uint8_t)ch; *sec = c1 > freq ? 1 : 2; return 0; }
      return -1;
    }
    p += 8;
  }
  return -1;
}

pid_t tune_spawn(const char *mon, uint8_t channel, uint8_t sec) {
  char ch[4]; const char *argv[8];
  if (tune_build_argv(mon, channel, sec, ch, argv) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    execvp("iw", (char *const *)argv);
    _exit(127);
  }
  return pid;
}

int tune_readback(const char *mon, uint8_t *channel, uint8_t *sec) {
  char cmd[64], buf[2048];
  snprintf(cmd, sizeof cmd, "iw dev %s info 2>/dev/null", mon);
  FILE *f = popen(cmd, "r");
  if (!f) return -1;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  pclose(f);
  return tune_parse_info(buf, channel, sec);
}
