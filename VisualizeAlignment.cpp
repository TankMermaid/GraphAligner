#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include "GfaGraph.h"
#include "AlignmentCorrectnessEstimation.h"
#include "CommonUtils.h"
#include "GraphAlignerWrapper.h"

void pad(std::string& str, size_t size)
{
	assert(str.size() <= size);
	while (str.size() < size)
	{
		str += " ";
	}
}

std::vector<AlignmentResult::TraceItem> loadTrace(std::string filename)
{
	std::ifstream file { filename };
	std::vector<AlignmentResult::TraceItem> result;
	while (file.good())
	{
		int nodeid, offset, reverse, readpos, type;
		char graphChar, readChar;
		file >> nodeid >> offset >> reverse >> readpos >> type >> graphChar >> readChar;
		if (!file.good()) break;
		result.emplace_back();
		result.back().nodeID = nodeid;
		result.back().offset = offset;
		result.back().reverse = reverse == 1;
		result.back().readpos = readpos;
		result.back().type = (AlignmentResult::TraceMatchType)type;
		result.back().graphChar = graphChar;
		result.back().readChar = readChar;
	}
	return result;
}

int main(int argc, char** argv)
{
	std::string tracefile { argv[1] };

	std::vector<AlignmentResult::TraceItem> trace = loadTrace(tracefile);

	std::string graphinfo;
	std::string graphpath;
	std::string alignmentinfo;
	std::string readinfo;
	std::string readpath;
	std::string slicewiseCorrectInfo;
	AlignmentCorrectnessEstimationState charwiseCorrect;
	AlignmentCorrectnessEstimationState slicewiseCorrect;
	std::vector<bool> charwiseCorrectCorrectTrace;
	std::vector<bool> charwiseCorrectFalseTrace;
	int oldNodeId = trace[0].nodeID;
	bool oldReverse = trace[0].reverse;
	int oldReadPos = trace[0].readpos;
	int readcharsUntilSlicewiseCheck = 64;
	int mismatches = 0;
	for (int i = 0; i < trace.size(); i++)
	{
		auto type = trace[i].type;
		char readChar = trace[i].readChar;
		char graphChar = trace[i].graphChar;
		if (i == 0)
		{
			graphinfo += "v";
			readinfo += "^";
		}
		if ((i > 0 && (trace[i].nodeID != trace[i-1].nodeID)) || type == AlignmentResult::TraceMatchType::FORWARDBACKWARDSPLIT)
		{
			int nodeidInfoLength = std::to_string(oldNodeId).size() + 1;
			if (i > graphinfo.size() + nodeidInfoLength)
			{
				graphinfo += std::to_string(oldNodeId);
				if (oldReverse) graphinfo += "-"; else graphinfo += "+";
			}
			int readSizeInfoLength = std::to_string(oldReadPos).size();
			if (i > readinfo.size() + readSizeInfoLength)
			{
				readinfo += std::to_string(oldReadPos);
			}
			pad(graphinfo, i);
			pad(readinfo, i);
			graphinfo += "v";
			readinfo += "^";
			oldNodeId = trace[i].nodeID;
			oldReverse = trace[i].reverse;
			oldReadPos = trace[i].readpos;
		}

		switch(type)
		{
			case AlignmentResult::TraceMatchType::MATCH:
				graphpath += graphChar;
				readpath += readChar;
				alignmentinfo += "|";
				assert(graphChar == readChar);
				readcharsUntilSlicewiseCheck--;
				break;
			case AlignmentResult::TraceMatchType::MISMATCH:
				graphpath += graphChar;
				readpath += readChar;
				alignmentinfo += " ";
				assert(graphChar != readChar);
				mismatches++;
				readcharsUntilSlicewiseCheck--;
				break;
			case AlignmentResult::TraceMatchType::INSERTION:
				graphpath += ' ';
				readpath += readChar;
				alignmentinfo += " ";
				mismatches++;
				readcharsUntilSlicewiseCheck--;
				break;
			case AlignmentResult::TraceMatchType::DELETION:
				graphpath += graphChar;
				readpath += ' ';
				mismatches++;
				alignmentinfo += " ";
				break;
			case AlignmentResult::TraceMatchType::FORWARDBACKWARDSPLIT:
				graphpath += graphChar;
				readpath += readChar;
				alignmentinfo += graphChar == readChar ? '|' : ' ';
				break;
		}

		if (readcharsUntilSlicewiseCheck == 0)
		{
			slicewiseCorrect = slicewiseCorrect.NextState(mismatches, 64);
			char addchar = slicewiseCorrect.CurrentlyCorrect() ? '#' : ' ';
			for (int i = 0; i < 64; i++)
			{
				slicewiseCorrectInfo += addchar;
			}
			mismatches = 0;
			readcharsUntilSlicewiseCheck = 64;
		}

		if (type == AlignmentResult::TraceMatchType::MATCH)
		{
			charwiseCorrect = charwiseCorrect.NextState(0, 1);
			charwiseCorrectCorrectTrace.push_back(charwiseCorrect.CorrectFromCorrect());
			charwiseCorrectFalseTrace.push_back(charwiseCorrect.FalseFromCorrect());
		}
		else if (type == AlignmentResult::TraceMatchType::FORWARDBACKWARDSPLIT)
		{
			bool oldCorrect = charwiseCorrect.CurrentlyCorrect();
			charwiseCorrect = AlignmentCorrectnessEstimationState {};
			charwiseCorrectCorrectTrace.push_back(oldCorrect);
			charwiseCorrectFalseTrace.push_back(oldCorrect);
			pad(slicewiseCorrectInfo, alignmentinfo.size());
			mismatches = 0;
			readcharsUntilSlicewiseCheck = 64;
			slicewiseCorrect = AlignmentCorrectnessEstimationState {};
		}
		else
		{
			charwiseCorrect = charwiseCorrect.NextState(1, 1);
			charwiseCorrectCorrectTrace.push_back(charwiseCorrect.CorrectFromCorrect());
			charwiseCorrectFalseTrace.push_back(charwiseCorrect.FalseFromCorrect());
		}
	}
	pad(slicewiseCorrectInfo, alignmentinfo.size());
	bool charwiseCurrentlyCorrect = charwiseCorrect.CurrentlyCorrect();
	std::string charwiseCorrectInfo = "";
	for (size_t i = charwiseCorrectCorrectTrace.size()-1; i < charwiseCorrectCorrectTrace.size(); i--)
	{
		if (charwiseCurrentlyCorrect)
		{
			charwiseCorrectInfo += "#";
			charwiseCurrentlyCorrect = charwiseCorrectCorrectTrace[i];
		}
		else
		{
			charwiseCorrectInfo += " ";
			charwiseCurrentlyCorrect = charwiseCorrectFalseTrace[i];
		}
	}
	std::reverse(charwiseCorrectInfo.begin(), charwiseCorrectInfo.end());
	std::cout << "       " << graphinfo << std::endl;
	std::cout << "GRAPH: " << graphpath << std::endl;
	std::cout << "       " << alignmentinfo << std::endl;
	std::cout << "READ:  " << readpath << std::endl;
	std::cout << "       " << readinfo << std::endl;
	std::cout << "       " << charwiseCorrectInfo << std::endl;
	std::cout << "       " << slicewiseCorrectInfo << std::endl;
}