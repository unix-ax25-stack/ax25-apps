/* ax25ipd.c     main entrypoint
 *
 * Copyright 1991, Michael Westerhof, Sun Microsystems, Inc.
 * This software may be freely used, distributed, or modified, providing
 * this header is not removed.
 *
 */

/*
 * cleaned up and prototyped for inclusion into the standard linux ax25
 * toolset in january 1997 by rob mayfield, vk5xxx/vk5zeu
 */

#include <stdio.h>
#include <signal.h>
#include <setjmp.h>

#include <netax25/daemon.h>
#include <config.h>

#include "../pathnames.h"
#include "ax25ipd.h"

jmp_buf restart_env;

/* Prototypes */
void hupper(int);

int
main(int argc, char **argv)
{
	if(setjmp(restart_env)==0) {
		signal(SIGHUP, hupper);
	}

	/* set up the handler for statistics reporting */
	signal(SIGUSR1, usr1_handler);
	signal(SIGINT, int_handler);
	signal(SIGTERM, term_handler);

	/* Say hello to the world */
	greet_world();

	/* Test arguments */
	if(argc>2){
		fprintf(stderr,"Usage: %s [<configuration-file>]\n",argv[0]);
		exit(1);
	}

	/* Initialize all routines */
	config_init();
	kiss_init();
	route_init();
	process_init();
	io_init();

	/* read config file */
	config_read(argv[1]);

	/* print the current config and route info */
	dump_config();
	dump_routes();
	dump_params();

	/* Open the IO stuff */
	io_open();

	/* if we get this far without error, let's fork off ! :-) */
	if (!daemon_start(TRUE)) {
		fprintf(stderr, "ax25ipd: cannot become a daemon\n");
		return 1;
	}

	/* and let the games begin */
	io_start();

	return(0);
}


void
greet_world()
{
	printf("\nax25ipd %s / %s\n", VERS2, VERSION);
	printf("Copyright 1991, Michael Westerhof, Sun Microsystems, Inc.\n");
	printf("This software may be freely used, distributed, or modified, providing\nthis header is not removed\n\n");
	fflush(stdout);
}

void
do_stats()
{
	int save_loglevel;

/* save the old loglevel, and force at least loglevel 1 */
	save_loglevel = loglevel;
	loglevel = 1;

	printf("\nSIGUSR1 signal: statistics and configuration report\n");

	greet_world();

	dump_config();
	dump_routes();
	dump_params();

	printf("\nInput stats:\n");
	printf("KISS input packets:  %d\n",stats.kiss_in);
	printf("           too big:  %d\n",stats.kiss_toobig);
	printf("          bad type:  %d\n",stats.kiss_badtype);
	printf("         too short:  %d\n",stats.kiss_tooshort);
	printf("        not for me:  %d\n",stats.kiss_not_for_me);
	printf("  I am destination:  %d\n",stats.kiss_i_am_dest);
	printf("    no route found:  %d\n",stats.kiss_no_ip_addr);
	printf("UDP  input packets:  %d\n",stats.udp_in);
	printf("IP   input packets:  %d\n",stats.ip_in);
	printf("   failed CRC test:  %d\n",stats.ip_failed_crc);
	printf("         too short:  %d\n",stats.ip_tooshort);
	printf("        not for me:  %d\n",stats.ip_not_for_me);
	printf("  I am destination:  %d\n",stats.ip_i_am_dest);
	printf("\nOutput stats:\n");
	printf("KISS output packets: %d\n",stats.kiss_out);
	printf("            beacons: %d\n",stats.kiss_beacon_outs);
	printf("UDP  output packets: %d\n",stats.udp_out);
	printf("IP   output packets: %d\n",stats.ip_out);
	printf("\n");

	fflush(stdout);

/* restore the old loglevel */
	loglevel = save_loglevel;
}

void
hupper(int i)
{
	printf("\nSIGHUP!\n");
	longjmp(restart_env, 1);
}

void
usr1_handler(int i)
{
	printf("\nSIGUSR1!\n");
	do_stats();
}

void
int_handler(int i)
{
	printf("\nSIGINT!\n");
	do_stats();
	exit(1);
}

void
term_handler(int i)
{
	printf("\nSIGTERM!\n");
	do_stats();
	exit(1);
}

