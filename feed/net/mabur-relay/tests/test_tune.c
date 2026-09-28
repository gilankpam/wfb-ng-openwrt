#include <string.h>
#include "t.h"
#include "tune.h"

static void t_valid_and_argv(void) {
  CHECK(tune_valid(36, 0)); CHECK(tune_valid(177, 2)); CHECK(tune_valid(136, 2));
  CHECK(!tune_valid(35, 0)); CHECK(!tune_valid(178, 0)); CHECK(!tune_valid(6, 0));
  CHECK(!tune_valid(136, 3));
  char ch[4]; const char *argv[8];
  CHECK_EQ(tune_build_argv("mon0", 136, 2, ch, argv), 0);
  const char *want[] = {"iw", "dev", "mon0", "set", "channel", "136", "HT40-"};
  for (int i = 0; i < 7; i++) CHECK(strcmp(argv[i], want[i]) == 0);
  CHECK(argv[7] == NULL);
  CHECK_EQ(tune_build_argv("mon0", 36, 1, ch, argv), 0);
  CHECK(strcmp(argv[5], "36") == 0); CHECK(strcmp(argv[6], "HT40+") == 0);
  CHECK_EQ(tune_build_argv("mon0", 149, 0, ch, argv), 0);
  CHECK(strcmp(argv[6], "HT20") == 0);
  CHECK_EQ(tune_build_argv("mon0", 136, 7, ch, argv), -1);
  uint8_t s;
  CHECK_EQ(tune_sec_code("HT40-", &s), 0); CHECK_EQ(s, 2);
  CHECK_EQ(tune_sec_code("HT40+", &s), 0); CHECK_EQ(s, 1);
  CHECK_EQ(tune_sec_code("HT20", &s), 0);  CHECK_EQ(s, 0);
  CHECK_EQ(tune_sec_code("VHT80", &s), -1);
}

static void t_parse_info(void) {
  uint8_t ch = 0, sec = 9;
  /* Captured verbatim from the CPE (2026-09-28). */
  const char *ht40m =
      "Interface mon0\n\tifindex 4\n\twdev 0x1\n\taddr dc:62:79:54:19:38\n\ttype monitor\n"
      "\tchannel 136 (5680 MHz), width: 40 MHz, center1: 5670 MHz\n\ttxpower 24.00 dBm\n";
  CHECK_EQ(tune_parse_info(ht40m, &ch, &sec), 0); CHECK_EQ(ch, 136); CHECK_EQ(sec, 2);
  const char *ht40p = "\tchannel 132 (5660 MHz), width: 40 MHz, center1: 5670 MHz\n";
  CHECK_EQ(tune_parse_info(ht40p, &ch, &sec), 0); CHECK_EQ(ch, 132); CHECK_EQ(sec, 1);
  const char *ht20 = "\tchannel 136 (5680 MHz), width: 20 MHz, center1: 5680 MHz\n";
  CHECK_EQ(tune_parse_info(ht20, &ch, &sec), 0); CHECK_EQ(ch, 136); CHECK_EQ(sec, 0);
  const char *noht = "\tchannel 149 (5745 MHz), width: 20 MHz (no HT), center1: 5745 MHz\n";
  CHECK_EQ(tune_parse_info(noht, &ch, &sec), 0); CHECK_EQ(ch, 149); CHECK_EQ(sec, 0);
  CHECK_EQ(tune_parse_info("Interface mon0\n\ttype monitor\n", &ch, &sec), -1);
  const char *w80 = "\tchannel 36 (5180 MHz), width: 80 MHz, center1: 5210 MHz\n";
  CHECK_EQ(tune_parse_info(w80, &ch, &sec), -1);
}

void t_tune(const char *fx) { (void)fx; t_valid_and_argv(); t_parse_info(); }
