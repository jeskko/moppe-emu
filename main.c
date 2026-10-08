/*
 * r58emu - command line front end.
 *
 *   r58emu [-n] [-c cu53|cu58] [-p p8e|p8n|l8m] [-v nvfile] rom.bin [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "r58.h"

static const char *evname[] = {
	[R58_EV_WDRESET] = "WDRESET", [R58_EV_NMI] = "NMI",
	[R58_EV_POWEROFF] = "POWEROFF", [R58_EV_POWERON] = "POWERON",
	[R58_EV_TX_ON] = "TX_ON", [R58_EV_TX_OFF] = "TX_OFF",
	[R58_EV_SYNTH] = "SYNTH", [R58_EV_MODEM_TX] = "MODEM_TX",
	[R58_EV_MBUS_TX] = "MBUS_TX", [R58_EV_GPS_TX] = "GPS_TX",
	[R58_EV_LCD] = "LCD",
};

int
main(int argc, char **argv)
{
	int card = R58_P8E, cu = R58_CU53AN, opt;
	const char *nv = 0;

	while ((opt = getopt(argc, argv, "c:p:v:")) != -1) {
		switch (opt) {
		case 'c': cu = !strcmp(optarg, "cu58") ? R58_CU58AF : R58_CU53AN; break;
		case 'p': card = !strcmp(optarg, "p8n") ? R58_P8N :
		                    !strcmp(optarg, "l8m") ? R58_L8M : R58_P8E; break;
		case 'v': nv = optarg; break;
		default:
			fprintf(stderr, "usage: r58emu [-c cu53|cu58] [-p p8e|p8n|l8m] [-v nv] rom [sec]\n");
			return 2;
		}
	}
	if (optind >= argc)
		return 2;
	static r58 m;
	r58_init(&m, card, cu);
	if (r58_load_rom(&m, argv[optind])) {
		perror(argv[optind]);
		return 1;
	}
	if (nv && r58_load_nv(&m, nv))
		fprintf(stderr, "warning: could not load %s\n", nv);
	double secs = optind + 1 < argc ? atof(argv[optind + 1]) : 3;
	int rc = r58_run(&m, secs);

	r58_event e;
	while (r58_next_event(&m, &e))
		if (e.type != R58_EV_SYNTH)
			printf("%10.6f %s %d\n", e.at / R58_XTAL_HZ, evname[e.type], e.arg);
	char up[32], lo[32];
	int icons = r58_display_text(&m, up, sizeof up, lo, sizeof lo);
	printf("stop=%d t=%.3f pc=%04x instr=%llu\n", rc, r58_time(&m), m.cpu.pc,
	       (unsigned long long)m.instructions);
	printf("display: [%s] [%s] icons=%04x\n", up, lo, icons);
	printf("synth: rx R=%u N=%u A=%u  tx R=%u N=%u A=%u  ctrl=%02x  RX LO %.4f MHz\n",
	       m.synth.rx_r, m.synth.rx_n, m.synth.rx_a, m.synth.tx_r, m.synth.tx_n,
	       m.synth.tx_a, m.synth.ctrl, r58_synth_vco_hz(&m, 0, 128, 12.8e6) / 1e6);
	printf("out0=%02x out1=%02x out2=%02x cpu_is_P8E=%d cu_is_alfa=%d\n",
	       m.out0, m.out1, m.out2, r58_peek(&m, 0xD015), r58_peek(&m, 0xD0DD));
	return 0;
}
