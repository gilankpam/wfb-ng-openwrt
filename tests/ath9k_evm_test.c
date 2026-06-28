/* Host unit test for the ath9k EVM helper. Build/run:
 *   gcc -O2 -Wall -o /tmp/ath9k_evm_test tests/ath9k_evm_test.c && /tmp/ath9k_evm_test
 * The helper below is the canonical source; it is copied verbatim into
 * patches/mac80211/999-ath9k-radiotap-evm.patch (ath9k/common.c hunk). */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

typedef uint8_t u8;
typedef uint32_t u32;

static u8 ath9k_cmn_evm_db(u32 e0, u32 e1, u32 e2, u32 e3, u32 e4)
{
	u32 words[4] = { e0, e1, e2, e3 };
	u32 sum = 0, n = 0;
	int w, b;

	for (w = 0; w < 4; w++)
		for (b = 0; b < 4; b++) {
			u8 v = (words[w] >> (8 * b)) & 0xff;
			if (v != 0x80 && v != 0) { sum += v; n++; }
		}
	/* evm4 carries only its low 2 bytes (status10 & 0xffff) */
	for (b = 0; b < 2; b++) {
		u8 v = (e4 >> (8 * b)) & 0xff;
		if (v != 0x80 && v != 0) { sum += v; n++; }
	}

	if (n == 0)
		return 0;                       /* no survivors -> absent */

	return (u8)(sum / n);                    /* |EVM| in dB, higher = better */
}

/* pack 4 bytes little-endian into a descriptor word */
static u32 W(u8 a, u8 b, u8 c, u8 d) { return a | (b << 8) | (c << 16) | ((u32)d << 24); }

int main(void)
{
	/* legacy frame: all 0x80 -> absent */
	assert(ath9k_cmn_evm_db(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 0);

	/* single survivor 30 -> 30 dB (no scaling, no clamp) */
	assert(ath9k_cmn_evm_db(W(30,0x80,0x80,0x80), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 30);

	/* HT20: 4 survivors of 20 -> 20 dB */
	assert(ath9k_cmn_evm_db(W(20,20,20,20), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 20);

	/* HT40: 6 survivors of 24 -> 24 dB */
	assert(ath9k_cmn_evm_db(W(24,24,24,24), W(24,0x80,0x80,24), 0x80808080u, 0x80808080u, 0x8080u) == 24);

	/* high reading: avg 40 -> 40 dB (NOT clamped) */
	assert(ath9k_cmn_evm_db(W(40,40,40,40), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 40);

	/* skip 0: zero bytes are unpopulated pilot slots, not 0 dB EVM.
	 * bytes [30,30,0,0] -> the two 0s skipped -> avg of two 30s -> 30 dB.
	 * (This is the short-frame fix: zeros must not drag the average down.) */
	assert(ath9k_cmn_evm_db(W(30,30,0,0), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 30);

	/* all-zero survivors -> none valid -> absent */
	assert(ath9k_cmn_evm_db(W(0,0x80,0x80,0x80), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 0);

	/* evm4 path: only its low 2 bytes count. low2 = [20,20] -> 20 dB */
	assert(ath9k_cmn_evm_db(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, W(20,20,0,0)) == 20);

	printf("ath9k_evm_test: all passed\n");
	return 0;
}
