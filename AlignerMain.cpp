#include <iostream>
#include <unistd.h>
#include <fstream>
#include "Aligner.h"
#include "stream.hpp"
#include "ThreadReadAssertion.h"

int main(int argc, char** argv)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

    struct sigaction act;
    act.sa_handler = ThreadReadAssertion::signal;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGSEGV, &act, 0);

    AlignerParams params;
	params.graphFile = "";
	params.fastqFile = "";
	params.alignmentFile = "";
	params.auggraphFile = "";
	params.seedFile = "";
	params.numThreads = 0;
	params.initialBandwidth = 0;
	params.rampBandwidth = 0;
	params.dynamicRowStart = 64;
	bool initialFullBand = false;
	int c;

	while ((c = getopt(argc, argv, "g:f:a:t:B:A:is:d:MSb:")) != -1)
	{
		switch(c)
		{
			case 'g':
				params.graphFile = std::string(optarg);
				break;
			case 'f':
				params.fastqFile = std::string(optarg);
				break;
			case 'a':
				params.alignmentFile = std::string(optarg);
				break;
			case 't':
				params.numThreads = std::stoi(optarg);
				break;
			case 'b':
				params.initialBandwidth = std::stoi(optarg);
				break;
			case 'B':
				params.rampBandwidth = std::stoi(optarg);
				break;
			case 'A':
				params.auggraphFile = std::string(optarg);
				break;
			case 'i':
				initialFullBand = true;
				break;
			case 's':
				params.seedFile = std::string(optarg);
				break;
			case 'd':
				params.dynamicRowStart = std::stoi(optarg);
				break;
		}
	}

	if (params.dynamicRowStart % 64 != 0)
	{
		std::cerr << "dynamic row start has to be a multiple of 64" << std::endl;
		std::exit(0);
	}

	if (params.numThreads < 1)
	{
		std::cerr << "number of threads must be >= 1" << std::endl;
		std::exit(0);
	}

	if (params.initialBandwidth < 2)
	{
		std::cerr << "bandwidth must be >= 2" << std::endl;
		std::exit(0);
	}

	if (params.rampBandwidth != 0 && params.rampBandwidth <= params.initialBandwidth)
	{
		std::cerr << "backup bandwidth must be higher than initial bandwidth" << std::endl;
		std::exit(0);
	}

	if (!initialFullBand && params.seedFile == "")
	{
		std::cerr << "either initial full band or seed file must be set" << std::endl;
		std::exit(0);
	}

	alignReads(params);

	return 0;
}
