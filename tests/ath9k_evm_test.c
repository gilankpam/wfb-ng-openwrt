/* Host unit test for the ath9k EVM-percent helper. Build: see Step 2.
 * The helper below is the canonical source; copy it verbatim into
 * patches/mac80211/1000-ath9k-radiotap-evm.patch (ath9k/common.c hunk). */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define EVM_K_NUM 3   /* byte->percent scale numerator   (3/1 = Realtek 3x|dB|) */
#define EVM_K_DEN 1   /* byte->percent scale denominator (3/2 if bytes are half-dB) */

static u8 ath9k_cmn_evm_percent(u32 e0, u32 e1, u32 e2, u32 e3, u32 e4)
{
	u32 words[4] = { e0, e1, e2, e3 };
	u32 sum = 0, n = 0, avg, pct;
	int w, b;

	for (w = 0; w < 4; w++)
		for (b = 0; b < 4; b++) {
			u8 v = (words[w] >> (8 * b)) & 0xff;
			if (v != 0x80) { sum += v; n++; }
		}
	/* evm4 carries only its low 2 bytes (status10 & 0xffff) */
	for (b = 0; b < 2; b++) {
		u8 v = (e4 >> (8 * b)) & 0xff;
		if (v != 0x80) { sum += v; n++; }
	}

	if (n == 0)
		return 0;                       /* no survivors -> absent */

	avg = sum / n;
	pct = (EVM_K_NUM * avg) / EVM_K_DEN;
	if (pct > 100)
		pct = 100;
	if (pct < 1)
		pct = 1;                        /* survivors exist -> real, low reading */
	return (u8)pct;
}

/* pack 4 bytes little-endian into a descriptor word */
static u32 W(u8 a, u8 b, u8 c, u8 d) { return a | (b << 8) | (c << 16) | ((u32)d << 24); }

int main(void)
{
	/* legacy frame: all 0x80 -> absent */
	assert(ath9k_cmn_evm_percent(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 0);

	/* single survivor 30 (|dB|=30) -> 3*30 = 90 */
	assert(ath9k_cmn_evm_percent(W(30,0x80,0x80,0x80), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 90);

	/* HT20: 4 survivors of 20 -> avg 20 -> 60 */
	assert(ath9k_cmn_evm_percent(W(20,20,20,20), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 60);

	/* HT40: 6 survivors of 24 -> avg 24 -> 72 */
	assert(ath9k_cmn_evm_percent(W(24,24,24,24), W(24,0x80,0x80,24), 0x80808080u, 0x80808080u, 0x8080u) == 72);

	/* clamp high: avg 40 -> 120 -> 100 */
	assert(ath9k_cmn_evm_percent(W(40,40,40,40), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 100);

	/* clamp low: single survivor 0 -> 0 -> 1 */
	assert(ath9k_cmn_evm_percent(W(0,0x80,0x80,0x80), 0x80808080u, 0x80808080u, 0x80808080u, 0x8080u) == 1);

	printf("ath9k_evm_test: all passed\n");
	return 0;
}
