#ifndef PCAP_H
#define PCAP_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t pcap_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* Little-endian classic pcap only (the committed fixture is written that way). */
static int pcap_load(const char *path, uint8_t *bufs[], size_t lens[], int max) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  uint8_t gh[24], rh[16];
  if (fread(gh, 1, 24, f) != 24 || pcap_le32(gh) != 0xa1b2c3d4u) { fclose(f); return -1; }
  int n = 0;
  while (n < max && fread(rh, 1, 16, f) == 16) {
    size_t len = pcap_le32(rh + 8);
    bufs[n] = malloc(len);
    if (!bufs[n] || fread(bufs[n], 1, len, f) != len) { fclose(f); return -1; }
    lens[n++] = len;
  }
  fclose(f);
  return n;
}
#endif
