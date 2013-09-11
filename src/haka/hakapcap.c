
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <libgen.h>

#include <haka/packet_module.h>
#include <haka/thread.h>
#include <haka/error.h>
#include <haka/version.h>
#include <haka/lua/state.h>

#include "app.h"
#include "thread.h"
#include "lua/state.h"


extern void packet_set_mode(enum packet_mode mode);

static void usage(FILE *output, const char *program)
{
	fprintf(stdout, "Usage: %s [options] <pcapfile> <config>\n", program);
}

static void help(const char *program)
{
	usage(stdout, program);

	fprintf(stdout, "Options:\n");
	fprintf(stdout, "\t-h,--help:       Display this information\n");
	fprintf(stdout, "\t--version:       Display version information\n");
	fprintf(stdout, "\t-d,--debug:      Display debug output\n");
	fprintf(stdout, "\t--pass-through:  Run in pass-through mode\n");
	fprintf(stdout, "\t-o <output>:     Save result in a pcap file\n");
}

static char *output = NULL;
static bool pass_throught = false;

static int parse_cmdline(int *argc, char ***argv)
{
	int c;
	int index = 0;

	static struct option long_options[] = {
		{ "version",      no_argument,       0, 'v' },
		{ "help",         no_argument,       0, 'h' },
		{ "debug",        no_argument,       0, 'd' },
		{ "pass-through", no_argument,       0, 'P' },
		{ 0,              0,                 0, 0 }
	};

	while ((c = getopt_long(*argc, *argv, "dho:", long_options, &index)) != -1) {
		switch (c) {
		case 'd':
			setlevel(HAKA_LOG_DEBUG, NULL);
			break;

		case 'h':
			help((*argv)[0]);
			return 0;

		case 'v':
			printf("version %s, arch %s, %s\n", HAKA_VERSION, HAKA_ARCH, HAKA_LUA);
			printf("API version %d\n", HAKA_API_VERSION);
			return 0;

		case 'P':
			pass_throught = true;
			break;

		case 'o':
			output = strdup(optarg);
			break;

		default:
			usage(stderr, (*argv)[0]);
			return 2;
		}
	}

	if (optind != *argc-2) {
		usage(stderr, (*argv)[0]);
		return 2;
	}

	*argc -= optind;
	*argv += optind;

	return -1;
}

int main(int argc, char *argv[])
{
	int ret;

	initialize();

	/* Check arguments */
	ret = parse_cmdline(&argc, &argv);
	if (ret >= 0) {
		clean_exit();
		free(output);
		return ret;
	}

	/* Select and initialize modules */
	{
		struct module *logger = module_load("log/stdout", NULL);
		if (!logger) {
			message(HAKA_LOG_WARNING, L"core", L"cannot log module");
			free(output);
			clean_exit();
			return 1;
		}

		set_log_module(logger);
		module_release(logger);

		struct module *pcap = NULL;
		if (output) pcap = module_load("packet/pcap", "-f", argv[0], "-o", output, NULL);
		else pcap = module_load("packet/pcap", "-f", argv[0], NULL);

		free(output);
		output = NULL;

		if (!pcap) {
			message(HAKA_LOG_FATAL, L"core", L"cannot load packet module");
			clean_exit();
			return 1;
		}

		set_packet_module(pcap);
		module_release(pcap);
		free(output);
	}

	/* Select configuration */
	{
		char *module_path;

		module_path = malloc(strlen(argv[1]) + 3);
		assert(module_path);
		strcpy(module_path, argv[1]);
		dirname(module_path);
		strcat(module_path, "/*");

		module_add_path(module_path);
		if (check_error()) {
			message(HAKA_LOG_FATAL, L"core", clear_error());
			free(module_path);
			clean_exit();
			exit(1);
		}

		free(module_path);
		module_path = NULL;

		set_configuration_script(argv[1]);
	}

	check();

	if (pass_throught) {
		messagef(HAKA_LOG_INFO, L"core", L"setting packet mode to pass-through\n");
		packet_set_mode(MODE_PASSTHROUGH);
	}

	/* Main loop */
	prepare(1);
	start();

	clean_exit();
	return 0;
}