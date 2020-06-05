#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include <fstream>

#include <mesytec-mvlc/mesytec-mvlc.h>
#include <fmt/format.h>
#include <lyra/lyra.hpp>

#include "mini_daq_lib.h"

using std::cout;
using std::cerr;
using std::endl;

using namespace mesytec::mvlc;

int main(int argc, char *argv[])
{
    // MVLC connection overrides
    std::string opt_mvlcEthHost;
    bool opt_mvlcUseFirstUSBDevice = false;
    int opt_mvlcUSBIndex = -1;
    std::string opt_mvlcUSBSerial;

    // listfile and run options
    bool opt_noListfile = false;
    std::string opt_listfileOut;
    std::string opt_listfileCompressionType = "zip";
    int opt_listfileCompressionLevel = 0;
    std::string opt_crateConfig;
    unsigned opt_secondsToRun = 10;

    bool opt_showHelp = false;


    auto cli
        = lyra::help(opt_showHelp)

        | lyra::opt(opt_mvlcEthHost, "hostname")
            ["--mvlc-eth"] ("mvlc ethernet hostname")

        | lyra::opt(opt_mvlcUseFirstUSBDevice)
            ["--mvlc-usb"] ("connect to the first mvlc usb device")

        | lyra::opt(opt_mvlcUSBIndex, "index")
            ["--mvlc-usb-index"] ("connect to the mvlc with the given usb device index")

        | lyra::opt(opt_mvlcUSBSerial, "serial")
            ["--mvlc-usb-serial"] ("connect to the mvlc with the given usb serial number")

        | lyra::opt(opt_noListfile)
            ["--no-listfile"] ("do not write readout data to a listfile")

        | lyra::opt(opt_listfileOut, "listfileName")
            ["--listfile"] ("filename of the output listfile")

        | lyra::opt(opt_listfileCompressionType, "type")
            ["--listfile-compression-type"].choices("zip", "lz4") ("'zip' or 'lz4'")

        | lyra::opt(opt_listfileCompressionLevel, "level")
            ["--listfile-compression-level"] ("compression level to use (for zip 0 means no compression)")

        | lyra::arg(opt_crateConfig, "crateConfig")
            ("crate config yaml file").required()

        | lyra::arg(opt_secondsToRun, "secondsToRun")
            ("duration of the DAQ run in seconds").required()
        ;

    auto cliParseResult = cli.parse({ argc, argv });

    if (!cliParseResult)
    {
        cerr << "Error parsing command line arguments: " << cliParseResult.errorMessage() << endl;
        return 1;
    }

    if (opt_showHelp)
    {
        cout << cli << endl;
        return 0;
    }

    std::ifstream inConfig(opt_crateConfig);

    if (!inConfig.is_open())
    {
        cerr << "Error opening crate config " << argv[1] << " for reading." << endl;
        return 1;
    }

    auto timeToRun = std::chrono::seconds(opt_secondsToRun);

    auto crateConfig = crate_config_from_yaml(inConfig);

    MVLC mvlc;

    if (!opt_mvlcEthHost.empty())
        mvlc = make_mvlc_eth(opt_mvlcEthHost);
    else if (opt_mvlcUseFirstUSBDevice)
        mvlc = make_mvlc_usb();
    else if (opt_mvlcUSBIndex >= 0)
        mvlc = make_mvlc_usb(opt_mvlcUSBIndex);
    else if (!opt_mvlcUSBSerial.empty())
        mvlc = make_mvlc_usb(opt_mvlcUSBSerial);
    else
        mvlc = make_mvlc(crateConfig);

    mvlc.setDisableTriggersOnConnect(true);

    if (auto ec = mvlc.connect())
    {
        cerr << "Error connecting to MVLC: " << ec.message() << endl;
        return 1;
    }

    cout << "Connected to MVLC " << mvlc.connectionInfo() << endl;

    //
    // init
    //
    {
        auto initResults = init_readout(mvlc, crateConfig);

        cout << "Results from init_commands:" << endl << initResults.init << endl;
        // cout << "Result from init_trigger_io:" << endl << initResults.triggerIO << endl;

        if (initResults.ec)
        {
            cerr << "Error running readout init sequence: " << initResults.ec.message() << endl;
            return 1;
        }
    }

    //
    // readout parser
    //
    const size_t BufferSize = util::Megabytes(1);
    const size_t BufferCount = 100;
    ReadoutBufferQueues snoopQueues(BufferSize, BufferCount);

    mini_daq::MiniDAQStats stats = {};
    auto parserCallbacks = make_mini_daq_callbacks(stats);

    auto parserState = readout_parser::make_readout_parser(crateConfig.stacks);
    Protected<readout_parser::ReadoutParserCounters> parserCounters({});

    std::thread parserThread;

    parserThread = std::thread(
        readout_parser::run_readout_parser,
        std::ref(parserState),
        std::ref(parserCounters),
        std::ref(snoopQueues),
        std::ref(parserCallbacks));

    //
    // readout
    //
    listfile::ZipCreator zipWriter;
    listfile::WriteHandle *lfh = nullptr;

    if (!opt_noListfile)
    {
        if (opt_listfileOut.empty())
            opt_listfileOut = opt_crateConfig + ".zip";

        cout << "Opening listfile " << opt_listfileOut << " for writing." << endl;

        zipWriter.createArchive(opt_listfileOut);

        if (opt_listfileCompressionType == "lz4")
            lfh = zipWriter.createLZ4Entry("listfile.mvlclst", opt_listfileCompressionLevel);
        else if (opt_listfileCompressionType == "zip")
            lfh = zipWriter.createZIPEntry("listfile.mvlclst", opt_listfileCompressionLevel);
        else
            return 1;
    }

    if (lfh)
        listfile::listfile_write_preamble(*lfh, crateConfig);

    ReadoutWorker readoutWorker(mvlc, crateConfig.triggers, snoopQueues, lfh);

    auto f = readoutWorker.start(timeToRun);

    // wait until readout done
    readoutWorker.waitableState().wait(
        [] (const ReadoutWorker::State &state)
        {
            return state == ReadoutWorker::State::Idle;
        });

    cout << "stopping readout_parser" << endl;

    // stop the readout parser
    if (parserThread.joinable())
    {
        cout << "waiting for empty snoopQueue buffer" << endl;
        auto sentinel = snoopQueues.emptyBufferQueue().dequeue(std::chrono::seconds(1));

        if (sentinel)
        {
            cout << "got a sentinel buffer for the readout parser" << endl;
            sentinel->clear();
            snoopQueues.filledBufferQueue().enqueue(sentinel);
            cout << "enqueued the sentinel buffer for the readout parser" << endl;
        }
        else
        {
            cout << "did not get an empty buffer from the snoopQueues" << endl;
        }

        parserThread.join();
    }

    int retval = 0;

    if (auto ec = disable_all_triggers(mvlc))
    {
        cerr << "Error disabling MVLC triggers: " << ec.message() << endl;
        retval = 1;
    }

    mvlc.disconnect();

    //
    // readout stats
    //
    {
        auto counters = readoutWorker.counters();

        auto tStart = counters.tStart;
        // Note: if idle this uses the tTerminateStart time point. This means
        // the duration from counters.tStart to counters.tTerminateStart is
        // used as the whole duration of the DAQ. This still does not correctly
        // reflect the actual data rate because it does contains the
        // bytes/packets/etc that where read during the terminate phase. It
        // should be closer than using tEnd though as that will include at
        // least one read timeout from the terminate procedure.
        auto tEnd = (counters.state != ReadoutWorker::State::Idle
                     ?  std::chrono::steady_clock::now()
                     : counters.tTerminateStart);
        auto runDuration = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart);
        double runSeconds = runDuration.count() / 1000.0;
        double megaBytes = counters.bytesRead * 1.0 / util::Megabytes(1);
        double mbs = megaBytes / runSeconds;

        cout << endl;
        cout << "---- readout stats ----" << endl;
        cout << "buffersRead=" << counters.buffersRead << endl;
        cout << "buffersFlushed=" << counters.buffersFlushed << endl;
        cout << "snoopMissedBuffers=" << counters.snoopMissedBuffers << endl;
        cout << "usbFramingErrors=" << counters.usbFramingErrors << endl;
        cout << "usbTempMovedBytes=" << counters.usbTempMovedBytes << endl;
        cout << "ethShortReads=" << counters.ethShortReads << endl;
        cout << "readTimeouts=" << counters.readTimeouts << endl;
        cout << "totalBytesTransferred=" << counters.bytesRead << endl;
        cout << "duration=" << runDuration.count() << " ms" << endl;

        cout << "stackHits: ";
        for (size_t stack=0; stack<counters.stackHits.size(); ++stack)
        {
            size_t hits = counters.stackHits[stack];

            if (hits)
                cout << stack << ": " << hits << " ";
        }
        cout << endl;

        cout << "stackErrors:";
        for (size_t stack=0; stack<counters.stackErrors.stackErrors.size(); ++stack)
        {
            const auto &errorCounts = counters.stackErrors.stackErrors[stack];

            for (auto it=errorCounts.begin(); it!=errorCounts.end(); ++it)
            {
                cout << fmt::format("stack={}, line={}, flags={}, count={}",
                                    stack, it->first.line, it->first.flags,
                                    it->second)
                    << endl;
            }
        }

        cout << endl;

        if (mvlc.connectionType() == ConnectionType::ETH)
        {
            auto pipeCounters = counters.ethStats[DataPipe];
            cout << endl;
            cout << "  -- eth data pipe receive stats --" << endl;
            cout << "  receiveAttempts=" << pipeCounters.receiveAttempts << endl;
            cout << "  receivedPackets=" << pipeCounters.receivedPackets << endl;
            cout << "  receivedBytes=" << pipeCounters.receivedBytes << endl;
            cout << "  shortPackets=" << pipeCounters.shortPackets << endl;
            cout << "  packetsWithResidue=" << pipeCounters.packetsWithResidue << endl;
            cout << "  noHeader=" << pipeCounters.noHeader << endl;
            cout << "  headerOutOfRange=" << pipeCounters.headerOutOfRange << endl;
            cout << "  lostPackets=" << pipeCounters.lostPackets << endl;
        }

        cout << endl;

        // listfile writer counters
        {
            auto writerCounters = counters.listfileWriterCounters;
            auto writerElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                writerCounters.tEnd - writerCounters.tStart);
            auto writerSeconds = writerElapsed.count() / 1000.0;
            double megaBytes = writerCounters.bytesWritten * 1.0 / util::Megabytes(1);
            double mbs = megaBytes / writerSeconds;

            cout << "  -- listfile writer counters --" << endl;
            cout << "  writes=" << writerCounters.writes << endl;
            cout << "  bytesWritten=" << writerCounters.bytesWritten << endl;
            cout << "  exception=";
            try
            {
                if (counters.eptr)
                    std::rethrow_exception(counters.eptr);
                cout << "none" << endl;
            }
            catch (const std::runtime_error &e)
            {
                cout << e.what() << endl;
            }
            cout << "  duration=" << writerSeconds << " s" << endl;
            cout << "  rate=" << mbs << " MB/s" << endl;
        }

        cout << endl;

        cout << "Ran for " << runSeconds << " seconds, transferred a total of " << megaBytes
            << " MB, resulting data rate: " << mbs << "MB/s"
            << endl;
    }

    //
    // parser stats
    //
    {
        auto counters = parserCounters.copy();

        cout << endl;
        cout << "---- readout parser stats ----" << endl;
        cout << "internalBufferLoss=" << counters.internalBufferLoss << endl;
        cout << "buffersProcessed=" << counters.buffersProcessed << endl;
        cout << "unusedBytes=" << counters.unusedBytes << endl;
        cout << "ethPacketLoss=" << counters.ethPacketLoss << endl;
        cout << "ethPacketsProcessed=" << counters.ethPacketsProcessed << endl;

        for (size_t sysEvent = 0; sysEvent < counters.systemEventTypes.size(); ++sysEvent)
        {
            if (counters.systemEventTypes[sysEvent])
            {
                auto sysEventName = system_event_type_to_string(sysEvent);

                cout << fmt::format("systemEventType {}, count={}",
                                    sysEventName, counters.systemEventTypes[sysEvent])
                    << endl;
            }
        }

        for (size_t pr=0; pr < counters.parseResults.size(); ++pr)
        {
            if (counters.parseResults[pr])
            {
                auto name = get_parse_result_name(static_cast<const readout_parser::ParseResult>(pr));

                cout << fmt::format("parseResult={}, count={}",
                                    name, counters.parseResults[pr])
                    << endl;
            }
        }

        cout << "parserExceptions=" << counters.parserExceptions << endl;

        dump_mini_daq_parser_stats(cout, stats);
    }

    return retval;
}
