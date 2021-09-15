#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mesytec-mvlc-c.h>

void print_help()
{
    printf("Options:\n");
    printf("  --crateconfig <filename>  # The CrateConfig YAML file to use\n");
    printf("  --mvlc-eth <hostname>     # Override the mvlc from the crateconfig\n");
    printf("  --mvlc-usb                # Override the mvlc from the crateconfig\n");
    printf("  --listfile <filename>     # Specify an output filename, e.g. run01.zip\n");
    printf("  --no-listfile             # Do not write a an output listfile\n");
    printf("  --overwrite-listfile      # Overwrite existing listfiles\n");
    printf("  --print-readout-data      # Print readout data\n");
    printf("  --duration <seconds>      # DAQ run duration in seconds\n");
}

int main(int argc, char *argv[])
{
    char *opt_mvlcEthHost = NULL;
    bool opt_useUSB = false;
    char *opt_crateConfigPath = NULL;
    char *opt_listfilePath = NULL;
    bool opt_noListfile = false;
    bool opt_overwriteListfile = false;
    bool opt_printReadoutData = false;
    int opt_runDuration = 5; // run duration in seconds

    // cli options
    while (1)
    {
        static struct option long_options[] =
        {
            {"mvlc-eth", required_argument, 0, 0 },
            {"mvlc-usb", no_argument, 0, 0 },
            {"listfile", required_argument, 0, 0 },
            {"no-listfile", no_argument, 0, 0 },
            {"overwrite-listfile", no_argument, 0, 0 },
            {"print-readout-data", no_argument, 0, 0 },
            {"crateconfig", required_argument, 0, 0 },
            {"duration", required_argument, 0, 0 },
            {"help", no_argument, 0, 0 },
            { 0, 0, 0, 0 }
        };

        int option_index = 0;

        int c = getopt_long(argc, argv, "", long_options, &option_index);

        if (c == -1)
            break;

        const char *optname = long_options[option_index].name;

        switch (c)
        {
            case 0:
                if (strcmp(optname, "crateconfig") == 0)
                    opt_crateConfigPath = optarg;
                else if (strcmp(optname, "mvlc-eth") == 0)
                    opt_mvlcEthHost = optarg;
                else if (strcmp(optname, "mvlc-usb") == 0)
                    opt_useUSB = true;
                else if (strcmp(optname, "listfile") == 0)
                    opt_listfilePath = optarg;
                else if (strcmp(optname, "no-listfile") == 0)
                    opt_noListfile = true;
                else if (strcmp(optname, "overwrite-listfile") == 0)
                    opt_overwriteListfile = true;
                else if (strcmp(optname, "print-readout-data") == 0)
                    opt_printReadoutData = true;
                else if (strcmp(optname, "duration") == 0)
                    opt_runDuration = atoi(optarg);
                else if (strcmp(optname, "help") == 0)
                {
                    print_help();
                    return 0;
                }

                break;

            case '?':
                print_help();
                return 1;
        }
    }

    if (!opt_crateConfigPath)
    {
        printf("Error: missing --crateconfig\n");
        return 1;
    }

    // crateconfig
    mvlc_crateconfig_t *crateconfig = mvlc_read_crateconfig_from_file(opt_crateConfigPath);

    // mvlc creation
    mvlc_ctrl_t *mvlc = NULL;

    if (opt_mvlcEthHost)
    {
        printf("Creating MVLC_ETH instance..\n");
        mvlc = mvlc_ctrl_create_eth(opt_mvlcEthHost);

    }
    else if (opt_useUSB)
    {
        printf("Creating MVLC_USB instance..\n");
        mvlc = mvlc_ctrl_create_usb();
    }
    else
    {
        printf("Creating MVLC from crateconfig..\n");
        mvlc = mvlc_ctrl_create_from_crateconfig(crateconfig);
    }

    static const size_t errbufsize = 1024;
    char errbuf[errbufsize];

    printf("Connecting to mvlc..\n");
    mvlc_err_t err = mvlc_ctrl_connect(mvlc);

    if (!mvlc_is_error(err))
    {
        printf("Connected to mvlc!\n");
    }
    else
    {
        printf("Error connecting to mvlc: %s\n", mvlc_format_error(err, errbuf, errbufsize));
        return 1;
    }

    // listfile setup
    mvlc_listfile_params_t listfileParams = make_default_listfile_params();

    if (opt_listfilePath)
        listfileParams.filepath = opt_listfilePath;

    if (opt_noListfile)
        listfileParams.writeListfile = false;

    if (opt_overwriteListfile)
        listfileParams.overwrite = true;

    // readout parser callbacks (TODO: opt_printReadoutData)
    readout_parser_callbacks_t parserCallbacks = {NULL, NULL};

    // readout
    mvlc_readout_t *rdo = mvlc_readout_create2(
            mvlc,
            crateconfig,
            listfileParams,
            parserCallbacks);

    MVLC_ReadoutState rdoState = get_readout_state(rdo);
    assert(rdoState == ReadoutState_Idle);

    err = mvlc_readout_start(rdo, opt_runDuration);

    if (!mvlc_is_error(err))
        printf("Readout started\n");
    else
    {
        printf("Error starting readout: %s\n", mvlc_format_error(err, errbuf, errbufsize));
        return 1;
    }

    // TODO: use readout_start, stop, pause, resume, check state in between
    // TODO: define and pass some callbacks for printing the data.


    while (get_readout_state(rdo) != ReadoutState_Idle)
    {
        usleep(100 * 1000);
    }

    printf("Readout done\n");


    mvlc_readout_destroy(rdo);
    mvlc_ctrl_destroy(mvlc);

    // ETH

    return 0;
}

