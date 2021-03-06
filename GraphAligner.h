#ifndef GraphAligner_H
#define GraphAligner_H

#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_set>
#include <queue>
#include <iostream>
#include "AlignmentGraph.h"
#include "vg.pb.h"
#include "NodeSlice.h"
#include "CommonUtils.h"
#include "GraphAlignerWrapper.h"
#include "AlignmentCorrectnessEstimation.h"
#include "ThreadReadAssertion.h"
#include "OrderedIndexKeeper.h"
#include "UniqueQueue.h"
#include "WordSlice.h"
#include "GraphAlignerCommon.h"

void printtime(const char* msg)
{
	static auto time = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	auto newtime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	std::cout << msg << " " << newtime << " (" << (newtime - time) << ")" << std::endl;
	time = newtime;
}

#ifndef NDEBUG
size_t getset(const std::set<size_t>& set, size_t pos)
{
	auto iter = set.begin();
	for (auto i = 0; i < pos; i++)
	{
		++iter;
	}
	return *iter;
}

size_t getset(const std::unordered_set<size_t>& set, size_t pos)
{
	auto iter = set.begin();
	for (auto i = 0; i < pos; i++)
	{
		++iter;
	}
	return *iter;
}
#endif

#ifndef NDEBUG
thread_local int debugLastRowMinScore;
#endif

template <typename LengthType, typename ScoreType, typename Word>
class GraphAligner
{
private:
	using WordSlice = typename WordContainer<LengthType, ScoreType, Word>::Slice;
	mutable BufferedWriter logger;
	typedef GraphAlignerParams<LengthType, ScoreType, Word> Params;
	const Params& params;
	typedef std::pair<LengthType, LengthType> MatrixPosition;
	class EqVector
	{
	public:
		EqVector(Word BA, Word BT, Word BC, Word BG) :
		BA(BA),
		BT(BT),
		BC(BC),
		BG(BG)
		{
		}
		Word getEq(char c) const
		{
			switch(c)
			{
				case 'A':
				case 'a':
					return BA;
				case 'T':
				case 't':
					return BT;
				case 'C':
				case 'c':
					return BC;
				case 'G':
				case 'g':
					return BG;
				case '-':
				default:
					assert(false);
			}
			assert(false);
			return 0;
		}
		Word BA;
		Word BT;
		Word BC;
		Word BG;
	};
	class DPSlice
	{
	public:
		DPSlice() :
		minScore(std::numeric_limits<ScoreType>::min()),
		minScoreIndex(),
		scores(),
		nodes(),
		correctness(),
		j(std::numeric_limits<LengthType>::max()),
		cellsProcessed(0),
		numCells(0)
		{}
		DPSlice(std::vector<typename NodeSlice<WordSlice>::MapItem>* vectorMap) :
		minScore(std::numeric_limits<ScoreType>::min()),
		minScoreIndex(),
		scores(vectorMap),
		nodes(),
		correctness(),
		j(std::numeric_limits<LengthType>::max()),
		cellsProcessed(0),
		numCells(0)
		{}
		ScoreType minScore;
		std::vector<LengthType> minScoreIndex;
		NodeSlice<WordSlice> scores;
		std::vector<size_t> nodes;
		AlignmentCorrectnessEstimationState correctness;
		LengthType j;
		size_t cellsProcessed;
		size_t numCells;
		size_t EstimatedMemoryUsage() const
		{
			return numCells * sizeof(typename WordContainer<LengthType, ScoreType, Word>::TinySlice) + scores.size() * (sizeof(size_t) * 3 + sizeof(int));
		}
		DPSlice getFrozenSqrtEndScores() const
		{
			DPSlice result;
			result.scores = scores.getFrozenSqrtEndScores();
			result.minScore = minScore;
			result.minScoreIndex = minScoreIndex;
			result.nodes = nodes;
			result.correctness = correctness;
			result.j = j;
			result.cellsProcessed = cellsProcessed;
			result.numCells = numCells;
			return result;
		}
		DPSlice getFrozenScores() const
		{
			DPSlice result;
			result.scores = scores.getFrozenScores();
			result.minScore = minScore;
			result.minScoreIndex = minScoreIndex;
			result.nodes = nodes;
			result.correctness = correctness;
			result.j = j;
			result.cellsProcessed = cellsProcessed;
			result.numCells = numCells;
			return result;
		}
	};
	class BacktraceOverride
	{
	public:
		class BacktraceItem
		{
		public:
			BacktraceItem(bool previousInSameRow, size_t previousIndex, MatrixPosition pos) :
			end(false),
			previousInSameRow(previousInSameRow),
			previousIndex(previousIndex),
			pos(pos)
			{}
			bool end;
			bool previousInSameRow;
			size_t previousIndex;
			MatrixPosition pos;
		};
		BacktraceOverride()
		{
		}
		BacktraceOverride(const Params& params, const std::string& sequence, const DPSlice& previous, const std::vector<DPSlice>& slices)
		{
			assert(slices.size() > 0);
			startj = slices[0].j;
			endj = slices.back().j;
			assert(endj == startj + (slices.size()-1) * WordConfiguration<Word>::WordSize);
			items.resize(WordConfiguration<Word>::WordSize * slices.size());
			makeTrace(params, sequence, previous, slices);
		};
		//returns the trace backwards, aka result[0] is at the bottom of the slice and result.back() at the top
		std::vector<MatrixPosition> GetBacktrace(MatrixPosition start) const
		{
			assert(items.size() > 0);
			assert(items.size() % WordConfiguration<Word>::WordSize == 0);
			assert(items.back().size() > 0);
			assert(items.back()[0].pos.second == start.second);
			size_t currentIndex = -1;
			size_t currentRow = items.size()-1;
			std::vector<MatrixPosition> result;
			for (size_t i = 0; i < items.back().size(); i++)
			{
				if (items.back()[i].pos == start)
				{
					currentIndex = i;
					break;
				}
			}
			assert(currentIndex != -1);
			while (true)
			{
				auto current = items[currentRow][currentIndex];
				assert(!current.end);
				result.push_back(current.pos);
				size_t nextIndex = current.previousIndex;
				size_t nextRow = current.previousInSameRow ? currentRow : currentRow - 1;
				if (nextRow == -1)
				{
					result.emplace_back(nextIndex, current.pos.second-1);
					break;
				}
				currentIndex = nextIndex;
				currentRow = nextRow;
			}
			return result;
		}
		LengthType startj;
		LengthType endj;
	private:
		void addReachableRec(const Params& params, MatrixPosition pos, size_t row, const std::string& sequence, const DPSlice& previous, const std::vector<DPSlice>& slices, std::vector<std::unordered_map<LengthType, size_t>>& indices)
		{
			assert(row < indices.size());
			if (indices[row].count(pos.first) == 1) return;
			auto size = indices[row].size();
			indices[row][pos.first] = size;
			if (row > 0 && row % WordConfiguration<Word>::WordSize == WordConfiguration<Word>::WordSize - 1)
			{
				size_t sliceIndex = row / WordConfiguration<Word>::WordSize;
				assert(sliceIndex < slices.size());
				auto nodeIndex = params.graph.IndexToNode(pos.first);
				assert(slices[sliceIndex].scores.hasNode(nodeIndex));
				auto nodeStart = params.graph.NodeStart(nodeIndex);
				auto offset = pos.first - nodeStart;
				if (!slices[sliceIndex].scores.node(nodeIndex)[offset].scoreEndExists) return;
			}
			assert(row == pos.second - slices[0].j);
			size_t sliceIndex = row / WordConfiguration<Word>::WordSize;
			MatrixPosition predecessor;
			if (sliceIndex > 0)
			{
				predecessor = pickBacktracePredecessor(params, sequence, slices[sliceIndex], pos, slices[sliceIndex-1]);
			}
			else
			{
				predecessor = pickBacktracePredecessor(params, sequence, slices[0], pos, previous);
			}
			assert(predecessor.second == pos.second || predecessor.second == pos.second-1);
			if (predecessor.second >= slices[0].j && predecessor.second != -1)
			{
				addReachableRec(params, predecessor, predecessor.second - slices[0].j, sequence, previous, slices, indices);
			}
		}
		void makeTrace(const Params& params, const std::string& sequence, const DPSlice& previous, const std::vector<DPSlice>& slices)
		{
			assert(slices.size() > 0);
			assert(items.size() == WordConfiguration<Word>::WordSize * slices.size());
			std::vector<std::unordered_map<LengthType, size_t>> indexOfPos;
			indexOfPos.resize(items.size());
			size_t endrow = items.size()-1;
#ifdef SLICEVERBOSE
			size_t numEndCells = 0;
#endif
			for (const auto& pair : slices.back().scores)
			{
				LengthType nodeStart = params.graph.NodeStart(pair.first);
				LengthType endj = slices.back().j + WordConfiguration<Word>::WordSize-1;
				for (size_t i = 0; i < pair.second.size(); i++)
				{
					if (pair.second[i].scoreEndExists)
					{
#ifdef SLICEVERBOSE
						numEndCells++;
#endif
						addReachableRec(params, MatrixPosition { nodeStart+i, endj }, endrow, sequence, previous, slices, indexOfPos);
					}
				}
			}
#ifdef SLICEVERBOSE
			std::cerr << " endcells " << numEndCells;
#endif

			for (size_t row = items.size()-1; row < items.size(); row--)
			{
				items[row].resize(indexOfPos[row].size(), BacktraceItem {false, 0, MatrixPosition {0, 0}});
				for (auto pair : indexOfPos[row])
				{
					auto w = pair.first;
					auto index = pair.second;
					MatrixPosition pos { w, slices[0].j + row };
					items[row][index].pos = pos;
					MatrixPosition predecessor;
					size_t sliceIndex = row / WordConfiguration<Word>::WordSize;
					if (row % WordConfiguration<Word>::WordSize == WordConfiguration<Word>::WordSize - 1)
					{
						auto nodeIndex = params.graph.IndexToNode(w);
						auto offset = w - params.graph.NodeStart(nodeIndex);
						assert(slices[sliceIndex].scores.hasNode(nodeIndex));
						if (!slices[sliceIndex].scores.node(nodeIndex)[offset].scoreEndExists)
						{
							items[row][index].end = true;
							continue;
						}
					}
					if (sliceIndex > 0)
					{
						predecessor = pickBacktracePredecessor(params, sequence, slices[sliceIndex], pos, slices[sliceIndex-1]);
					}
					else
					{
						predecessor = pickBacktracePredecessor(params, sequence, slices[0], pos, previous);
					}
					if (predecessor.second == pos.second)
					{
						items[row][index].previousInSameRow = true;
						items[row][index].previousIndex = indexOfPos[row].at(predecessor.first);
					}
					else
					{
						items[row][index].previousInSameRow = false;
						if (row != 0)
						{
							items[row][index].previousIndex = indexOfPos[row-1].at(predecessor.first);
						}
						else
						{
							items[row][index].previousIndex = predecessor.first;
						}
					}
				}
#ifndef NDEBUG
				for (size_t i = 0; i < items[row].size(); i++)
				{
					assert(items[row][i].end || items[row][i].pos.first != 0);
				}
#endif
			}
		}
		std::vector<std::vector<BacktraceItem>> items;
	};
	class DPTable
	{
	public:
		DPTable() :
		slices(),
		samplingFrequency(0)
		{}
		std::vector<DPSlice> slices;
		size_t samplingFrequency;
		std::vector<LengthType> bandwidthPerSlice;
		std::vector<AlignmentCorrectnessEstimationState> correctness;
		std::vector<BacktraceOverride> backtraceOverrides;
	};
	class TwoDirectionalSplitAlignment
	{
	public:
		size_t EstimatedCorrectlyAligned() const
		{
			return (forward.bandwidthPerSlice.size() + backward.bandwidthPerSlice.size()) * WordConfiguration<Word>::WordSize;
		}
		size_t sequenceSplitIndex;
		DPTable forward;
		DPTable backward;
	};
public:

	GraphAligner(const Params& params) :
	logger(std::cerr),
	params(params)
	{
	}
	
	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, LengthType dynamicRowStart) const
	{
		std::vector<typename NodeSlice<WordSlice>::MapItem> nodesliceMap;
		nodesliceMap.resize(params.graph.NodeSize(), {0, 0, 0});
		auto timeStart = std::chrono::system_clock::now();
		assert(params.graph.finalized);
		auto trace = getBacktraceFullStart(sequence, nodesliceMap);
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		//failed alignment, don't output
		if (std::get<0>(trace) == std::numeric_limits<ScoreType>::max()) return emptyAlignment(time, std::get<2>(trace));
		if (std::get<1>(trace).size() == 0) return emptyAlignment(time, std::get<2>(trace));
		auto result = traceToAlignment(seq_id, sequence, std::get<0>(trace), std::get<1>(trace), std::get<2>(trace));
		result.alignmentStart = std::get<1>(trace)[0].second;
		result.alignmentEnd = std::get<1>(trace).back().second;
		timeEnd = std::chrono::system_clock::now();
		time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		result.elapsedMilliseconds = time;
		return result;
	}

	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, LengthType dynamicRowStart, const std::vector<std::tuple<int, size_t, bool>>& seedHits) const
	{
		auto timeStart = std::chrono::system_clock::now();
		assert(params.graph.finalized);
		assert(seedHits.size() > 0);
		size_t bestAlignmentEstimatedCorrectlyAligned;
		std::tuple<int, size_t, bool> bestSeed;
		std::vector<std::tuple<size_t, size_t, size_t>> triedAlignmentNodes;
		std::pair<std::tuple<ScoreType, std::vector<MatrixPosition>>, std::tuple<ScoreType, std::vector<MatrixPosition>>> bestTrace;
		bool hasAlignment = false;
		std::vector<typename NodeSlice<WordSlice>::MapItem> nodesliceMap;
		nodesliceMap.resize(params.graph.NodeSize(), {0, 0, 0});
		for (size_t i = 0; i < seedHits.size(); i++)
		{
			logger << "seed " << i << "/" << seedHits.size() << " " << std::get<0>(seedHits[i]) << (std::get<2>(seedHits[i]) ? "-" : "+") << "," << std::get<1>(seedHits[i]);
			auto nodeIndex = params.graph.nodeLookup.at(std::get<0>(seedHits[i]) * 2);
			auto pos = std::get<1>(seedHits[i]);
			if (std::any_of(triedAlignmentNodes.begin(), triedAlignmentNodes.end(), [nodeIndex, pos](auto triple) { return std::get<0>(triple) <= pos && std::get<1>(triple) >= pos && std::get<2>(triple) == nodeIndex; }))
			{
				logger << "seed " << i << " already aligned" << BufferedWriter::Flush;
				continue;
			}
			logger << BufferedWriter::Flush;
			auto alignment = getSplitAlignment(sequence, std::get<0>(seedHits[i]), std::get<2>(seedHits[i]), std::get<1>(seedHits[i]), sequence.size() * 0.4, nodesliceMap);
			auto trace = getPiecewiseTracesFromSplit(alignment, sequence, nodesliceMap);
			addAlignmentNodes(triedAlignmentNodes, trace, alignment.sequenceSplitIndex);
			if (!hasAlignment)
			{
				bestTrace = std::move(trace);
				bestSeed = seedHits[i];
				hasAlignment = true;
				bestAlignmentEstimatedCorrectlyAligned = alignment.EstimatedCorrectlyAligned();
			}
			else
			{
				if (alignment.EstimatedCorrectlyAligned() > bestAlignmentEstimatedCorrectlyAligned)
				{
					bestTrace = std::move(trace);
					bestAlignmentEstimatedCorrectlyAligned = alignment.EstimatedCorrectlyAligned();
					bestSeed = seedHits[i];
				}
			}
		}
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		//failed alignment, don't output
		if (!hasAlignment)
		{
			return emptyAlignment(time, 0);
		}
		if (std::get<0>(bestTrace.first) == std::numeric_limits<ScoreType>::max() && std::get<0>(bestTrace.second) == std::numeric_limits<ScoreType>::max())
		{
			return emptyAlignment(time, 0);
		}

		auto traceVector = getTraceInfo(sequence, std::get<1>(bestTrace.second), std::get<1>(bestTrace.first));

		auto fwresult = traceToAlignment(seq_id, sequence, std::get<0>(bestTrace.first), std::get<1>(bestTrace.first), 0);
		auto bwresult = traceToAlignment(seq_id, sequence, std::get<0>(bestTrace.second), std::get<1>(bestTrace.second), 0);
		//failed alignment, don't output
		if (fwresult.alignmentFailed && bwresult.alignmentFailed)
		{
			return emptyAlignment(time, 0);
		}
		auto result = mergeAlignments(bwresult, fwresult);
		result.trace = traceVector;
		LengthType lastAligned = 0;
		if (std::get<1>(bestTrace.second).size() > 0)
		{
			lastAligned = std::get<1>(bestTrace.second)[0].second;
		}
		else
		{
			lastAligned = std::get<1>(bestSeed);
			assert(std::get<1>(bestTrace.first).size() > 0);
		}
		result.alignment.set_query_position(lastAligned);
		result.alignmentStart = lastAligned;
		result.alignmentEnd = result.alignmentStart + bestAlignmentEstimatedCorrectlyAligned;
		timeEnd = std::chrono::system_clock::now();
		time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		result.elapsedMilliseconds = time;
		return result;
	}

	static MatrixPosition pickBacktracePredecessor(const Params& params, const std::string& sequence, const DPSlice& slice, const MatrixPosition pos, const DPSlice& previousSlice)
	{
		assert(pos.second >= slice.j);
		assert(pos.second < slice.j + WordConfiguration<Word>::WordSize);
		auto nodeIndex = params.graph.IndexToNode(pos.first);
		assert(slice.scores.hasNode(nodeIndex));
		auto scoreHere = getValue(params, slice, pos.second - slice.j, pos.first);
		if (pos.second == 0 && previousSlice.scores.hasNode(nodeIndex) && (scoreHere == 0 || scoreHere == 1)) return { pos.first, pos.second - 1 };
		if (pos.first == params.graph.NodeStart(nodeIndex))
		{
			for (auto neighbor : params.graph.inNeighbors[nodeIndex])
			{
				LengthType u = params.graph.NodeEnd(neighbor)-1;
				auto horizontalScore = getValueOrMax(params, slice, pos.second - slice.j, u, sequence.size());
				assert(horizontalScore >= scoreHere-1);
				if (horizontalScore == scoreHere-1)
				{
					return { u, pos.second };
				}
				ScoreType diagonalScore;
				if (pos.second == slice.j)
				{
					diagonalScore = getValueOrMax(params, previousSlice, WordConfiguration<Word>::WordSize - 1, u, sequence.size());
				}
				else
				{
					diagonalScore = getValueOrMax(params, slice, pos.second - 1 - slice.j, u, sequence.size());
				}
				if (characterMatch(sequence[pos.second], params.graph.NodeSequences(pos.first)))
				{
					assert(diagonalScore >= scoreHere);
					if (diagonalScore == scoreHere)
					{
						return { u, pos.second - 1 };
					}
				}
				else
				{
					assert(diagonalScore >= scoreHere-1);
					if (diagonalScore == scoreHere-1)
					{
						return { u, pos.second - 1 };
					}
				}
			}
		}
		else
		{
			auto horizontalScore = getValueOrMax(params, slice, pos.second - slice.j, pos.first-1, sequence.size());
			assert(horizontalScore >= scoreHere-1);
			if (horizontalScore == scoreHere-1)
			{
				return { pos.first - 1, pos.second };
			}
			ScoreType diagonalScore;
			if (pos.second == slice.j)
			{
				diagonalScore = getValueOrMax(params, previousSlice, WordConfiguration<Word>::WordSize - 1, pos.first-1, sequence.size());
			}
			else
			{
				diagonalScore = getValueOrMax(params, slice, pos.second - 1 - slice.j, pos.first-1, sequence.size());
			}
			if (characterMatch(sequence[pos.second], params.graph.NodeSequences(pos.first)))
			{
				assert(diagonalScore >= scoreHere);
				if (diagonalScore == scoreHere)
				{
					return { pos.first - 1, pos.second - 1 };
				}
			}
			else
			{
				assert(diagonalScore >= scoreHere-1);
				if (diagonalScore == scoreHere-1)
				{
					return { pos.first - 1, pos.second - 1 };
				}
			}
		}
		ScoreType scoreUp;
		if (pos.second == slice.j)
		{
			assert(previousSlice.j + WordConfiguration<Word>::WordSize == slice.j);
			scoreUp = getValueOrMax(params, previousSlice, WordConfiguration<Word>::WordSize - 1, pos.first, sequence.size());
		}
		else
		{
			scoreUp = getValueOrMax(params, slice, pos.second - 1 - slice.j, pos.first, sequence.size());
		}
		assert(scoreUp >= scoreHere-1);
		if (scoreUp == scoreHere - 1)
		{
			return { pos.first, pos.second - 1 };
		}
		assert(false);
		std::abort();
		return pos;
	}
private:

	void addAlignmentNodes(std::vector<std::tuple<size_t, size_t, size_t>>& tried, const std::pair<std::tuple<ScoreType, std::vector<MatrixPosition>>, std::tuple<ScoreType, std::vector<MatrixPosition>>>& trace, LengthType sequenceSplitIndex) const
	{
		if (std::get<1>(trace.first).size() > 0)
		{
			size_t oldNodeIndex = params.graph.IndexToNode(std::get<1>(trace.first)[0].first);
			size_t startIndex = std::get<1>(trace.first)[0].second;
			size_t endIndex = std::get<1>(trace.first)[0].second;
			for (size_t i = 1; i < std::get<1>(trace.first).size(); i++)
			{
				size_t nodeIndex = params.graph.IndexToNode(std::get<1>(trace.first)[i].first);
				size_t index = std::get<1>(trace.first)[i].second;
				if (nodeIndex != oldNodeIndex)
				{
					tried.emplace_back(startIndex, endIndex, oldNodeIndex);
					startIndex = index;
					oldNodeIndex = nodeIndex;
				}
				endIndex = index;
			}
			tried.emplace_back(startIndex, endIndex, oldNodeIndex);
		}
		if (std::get<1>(trace.second).size() > 0)
		{
			size_t oldNodeIndex = params.graph.IndexToNode(std::get<1>(trace.second)[0].first);
			size_t startIndex = std::get<1>(trace.second)[0].second;
			size_t endIndex = std::get<1>(trace.second)[0].second;
			for (size_t i = 1; i < std::get<1>(trace.second).size(); i++)
			{
				size_t nodeIndex = params.graph.IndexToNode(std::get<1>(trace.second)[i].first);
				size_t index = std::get<1>(trace.second)[i].second;
				if (nodeIndex != oldNodeIndex)
				{
					tried.emplace_back(startIndex, endIndex, oldNodeIndex);
					startIndex = index;
					oldNodeIndex = nodeIndex;
				}
				endIndex = index;
			}
			tried.emplace_back(startIndex, endIndex, oldNodeIndex);
		}
	}

	AlignmentResult emptyAlignment(size_t elapsedMilliseconds, size_t cellsProcessed) const
	{
		vg::Alignment result;
		result.set_score(std::numeric_limits<decltype(result.score())>::max());
		return AlignmentResult { result, true, cellsProcessed, elapsedMilliseconds };
	}

	bool posEqual(const vg::Position& pos1, const vg::Position& pos2) const
	{
		return pos1.node_id() == pos2.node_id() && pos1.is_reverse() == pos2.is_reverse();
	}

	AlignmentResult mergeAlignments(const AlignmentResult& first, const AlignmentResult& second) const
	{
		assert(!first.alignmentFailed || !second.alignmentFailed);
		if (first.alignmentFailed) return second;
		if (second.alignmentFailed) return first;
		if (first.alignment.path().mapping_size() == 0) return second;
		if (second.alignment.path().mapping_size() == 0) return first;
		assert(!first.alignmentFailed);
		assert(!second.alignmentFailed);
		AlignmentResult finalResult;
		finalResult.alignmentFailed = false;
		finalResult.cellsProcessed = first.cellsProcessed + second.cellsProcessed;
		finalResult.elapsedMilliseconds = first.elapsedMilliseconds + second.elapsedMilliseconds;
		finalResult.alignment = first.alignment;
		finalResult.alignment.set_score(first.alignment.score() + second.alignment.score());
		int start = 0;
		auto firstEndPos = first.alignment.path().mapping(first.alignment.path().mapping_size()-1).position();
		auto secondStartPos = second.alignment.path().mapping(0).position();
		auto firstEndPosNodeId = params.graph.nodeLookup.at(firstEndPos.node_id());
		auto secondStartPosNodeId = params.graph.nodeLookup.at(secondStartPos.node_id());
		if (posEqual(firstEndPos, secondStartPos))
		{
			start = 1;
		}
		else if (std::find(params.graph.outNeighbors[firstEndPosNodeId].begin(), params.graph.outNeighbors[firstEndPosNodeId].end(), secondStartPosNodeId) != params.graph.outNeighbors[firstEndPosNodeId].end())
		{
			start = 0;
		}
		else
		{
			logger << "Piecewise alignments can't be merged!";
			logger << " first end: " << firstEndPos.node_id() << " " << (firstEndPos.is_reverse() ? "-" : "+");
			logger << " second start: " << secondStartPos.node_id() << " " << (secondStartPos.is_reverse() ? "-" : "+") << BufferedWriter::Flush;
		}
		for (int i = start; i < second.alignment.path().mapping_size(); i++)
		{
			auto mapping = finalResult.alignment.mutable_path()->add_mapping();
			*mapping = second.alignment.path().mapping(i);
		}
		return finalResult;
	}

	std::vector<AlignmentResult::TraceItem> getTraceInfo(const std::string& sequence, const std::vector<MatrixPosition>& bwtrace, const std::vector<MatrixPosition>& fwtrace) const
	{
		std::vector<AlignmentResult::TraceItem> result;
		if (bwtrace.size() > 0)
		{
			auto bw = getTraceInfoInner(sequence, bwtrace);
			result.insert(result.end(), bw.begin(), bw.end());
		}
		if (bwtrace.size() > 0 && fwtrace.size() > 0)
		{
			auto nodeid = params.graph.IndexToNode(fwtrace[0].first);
			result.emplace_back();
			result.back().type = AlignmentResult::TraceMatchType::FORWARDBACKWARDSPLIT;
			result.back().nodeID = params.graph.nodeIDs[nodeid] / 2;
			result.back().reverse = nodeid % 2 == 1;
			result.back().offset = fwtrace[0].first - params.graph.NodeStart(nodeid);
			result.back().readpos = fwtrace[0].second;
			result.back().graphChar = params.graph.NodeSequences(fwtrace[0].first);
			result.back().readChar = sequence[fwtrace[0].second];
		}
		if (fwtrace.size() > 0)
		{
			auto fw = getTraceInfoInner(sequence, fwtrace);
			result.insert(result.end(), fw.begin(), fw.end());
		}
		return result;
	}

	std::vector<AlignmentResult::TraceItem> getTraceInfoInner(const std::string& sequence, const std::vector<MatrixPosition>& trace) const
	{
		std::vector<AlignmentResult::TraceItem> result;
		for (size_t i = 1; i < trace.size(); i++)
		{
			auto newpos = trace[i];
			auto oldpos = trace[i-1];
			assert(newpos.second == oldpos.second || newpos.second == oldpos.second+1);
			assert(newpos.second != oldpos.second || newpos.first != oldpos.first);
			auto oldNodeIndex = params.graph.IndexToNode(oldpos.first);
			auto newNodeIndex = params.graph.IndexToNode(newpos.first);
			if (oldpos.first == params.graph.NodeEnd(oldNodeIndex)-1)
			{
				assert(newpos.first == oldpos.first || newpos.first == params.graph.NodeStart(newNodeIndex));
			}
			else
			{
				assert(newpos.first == oldpos.first || newpos.first == oldpos.first+1);
			}
			bool diagonal = true;
			if (newpos.second == oldpos.second) diagonal = false;
			if (newpos.first == oldpos.first)
			{
				auto newNodeIndex = params.graph.IndexToNode(newpos.first);
				if (newpos.second == oldpos.second+1 && params.graph.NodeEnd(newNodeIndex) == params.graph.NodeStart(newNodeIndex)+1 && std::find(params.graph.outNeighbors[newNodeIndex].begin(), params.graph.outNeighbors[newNodeIndex].end(), newNodeIndex) != params.graph.outNeighbors[newNodeIndex].end())
				{
					//one node self-loop, diagonal is valid
				}
				else
				{
					diagonal = false;
				}
			}
			result.emplace_back();
			result.back().nodeID = params.graph.nodeIDs[newNodeIndex] / 2;
			result.back().reverse = params.graph.nodeIDs[newNodeIndex] % 2 == 1;
			result.back().offset = newpos.first - params.graph.NodeStart(newNodeIndex);
			result.back().readpos = newpos.second;
			result.back().graphChar = params.graph.NodeSequences(newpos.first);
			result.back().readChar = sequence[newpos.second];
			if (newpos.second == oldpos.second)
			{
				result.back().type = AlignmentResult::TraceMatchType::DELETION;
			}
			else if (newpos.first == oldpos.first && !diagonal)
			{
				result.back().type = AlignmentResult::TraceMatchType::INSERTION;
			}
			else
			{
				assert(diagonal);
				if (characterMatch(sequence[newpos.second], params.graph.NodeSequences(newpos.first)))
				{
					result.back().type = AlignmentResult::TraceMatchType::MATCH;
				}
				else
				{
					result.back().type = AlignmentResult::TraceMatchType::MISMATCH;
				}
			}
		}
		return result;
	}

	AlignmentResult traceToAlignment(const std::string& seq_id, const std::string& sequence, ScoreType score, const std::vector<MatrixPosition>& trace, size_t cellsProcessed) const
	{
		vg::Alignment result;
		result.set_name(seq_id);
		result.set_score(score);
		result.set_sequence(sequence);
		auto path = new vg::Path;
		result.set_allocated_path(path);
		if (trace.size() == 0) return AlignmentResult { result, true, cellsProcessed, std::numeric_limits<size_t>::max() };
		size_t pos = 0;
		size_t oldNode = params.graph.IndexToNode(trace[0].first);
		while (oldNode == params.graph.dummyNodeStart)
		{
			pos++;
			if (pos == trace.size()) return emptyAlignment(std::numeric_limits<size_t>::max(), cellsProcessed);
			assert(pos < trace.size());
			assert(trace[pos].second >= trace[pos-1].second);
			oldNode = params.graph.IndexToNode(trace[pos].first);
			assert(oldNode < params.graph.nodeIDs.size());
		}
		if (oldNode == params.graph.dummyNodeEnd) return emptyAlignment(std::numeric_limits<size_t>::max(), cellsProcessed);
		int rank = 0;
		auto vgmapping = path->add_mapping();
		auto position = new vg::Position;
		vgmapping->set_allocated_position(position);
		vgmapping->set_rank(rank);
		position->set_node_id(params.graph.nodeIDs[oldNode]);
		position->set_is_reverse(params.graph.reverse[oldNode]);
		position->set_offset(trace[pos].first - params.graph.NodeStart(oldNode));
		MatrixPosition btNodeStart = trace[pos];
		MatrixPosition btNodeEnd = trace[pos];
		MatrixPosition btBeforeNode = trace[pos];
		for (; pos < trace.size(); pos++)
		{
			if (params.graph.IndexToNode(trace[pos].first) == params.graph.dummyNodeEnd) break;
			if (params.graph.IndexToNode(trace[pos].first) == oldNode)
			{
				btNodeEnd = trace[pos];
				continue;
			}
			assert(trace[pos].second >= trace[pos-1].second);
			assert(params.graph.IndexToNode(btNodeEnd.first) == params.graph.IndexToNode(btNodeStart.first));
			assert(btNodeEnd.second >= btNodeStart.second);
			assert(btNodeEnd.first >= btNodeStart.first);
			auto edit = vgmapping->add_edit();
			edit->set_from_length(btNodeEnd.first - btNodeStart.first + 1);
			edit->set_to_length(btNodeEnd.second - btBeforeNode.second);
			edit->set_sequence(sequence.substr(btNodeStart.second, btNodeEnd.second - btBeforeNode.second));
			oldNode = params.graph.IndexToNode(trace[pos].first);
			btBeforeNode = btNodeEnd;
			btNodeStart = trace[pos];
			btNodeEnd = trace[pos];
			rank++;
			vgmapping = path->add_mapping();
			position = new vg::Position;
			vgmapping->set_allocated_position(position);
			vgmapping->set_rank(rank);
			position->set_node_id(params.graph.nodeIDs[oldNode]);
			position->set_is_reverse(params.graph.reverse[oldNode]);
		}
		auto edit = vgmapping->add_edit();
		edit->set_from_length(btNodeEnd.first - btNodeStart.first);
		edit->set_to_length(btNodeEnd.second - btBeforeNode.second);
		edit->set_sequence(sequence.substr(btNodeStart.second, btNodeEnd.second - btBeforeNode.second));
		return AlignmentResult { result, false, cellsProcessed, std::numeric_limits<size_t>::max() };
	}

#ifndef NDEBUG
	void verifyTrace(const std::vector<MatrixPosition>& trace, const std::string& sequence, volatile ScoreType score, const DPTable& band) const
	{
		volatile ScoreType realscore = 0;
		assert(trace[0].second == 0);
		realscore += characterMatch(sequence[0], params.graph.NodeSequences(trace[0].first)) ? 0 : 1;
		for (size_t i = 1; i < trace.size(); i++)
		{
			auto newpos = trace[i];
			auto oldpos = trace[i-1];
			assert(newpos.second == oldpos.second || newpos.second == oldpos.second+1);
			assert(newpos.second != oldpos.second || newpos.first != oldpos.first);
			auto oldNodeIndex = params.graph.IndexToNode(oldpos.first);
			if (oldpos.first == params.graph.NodeEnd(oldNodeIndex)-1)
			{
				auto newNodeIndex = params.graph.IndexToNode(newpos.first);
				assert(newpos.first == oldpos.first || newpos.first == params.graph.NodeStart(newNodeIndex));
			}
			else
			{
				assert(newpos.first == oldpos.first || newpos.first == oldpos.first+1);
			}
			bool diagonal = true;
			if (newpos.second == oldpos.second) diagonal = false;
			if (newpos.first == oldpos.first)
			{
				auto newNodeIndex = params.graph.IndexToNode(newpos.first);
				if (newpos.second == oldpos.second+1 && params.graph.NodeEnd(newNodeIndex) == params.graph.NodeStart(newNodeIndex)+1 && std::find(params.graph.outNeighbors[newNodeIndex].begin(), params.graph.outNeighbors[newNodeIndex].end(), newNodeIndex) != params.graph.outNeighbors[newNodeIndex].end())
				{
					//one node self-loop, diagonal is valid
				}
				else
				{
					diagonal = false;
				}
			}
			if (!diagonal || !characterMatch(sequence[newpos.second], params.graph.NodeSequences(newpos.first)))
			{
				realscore++;
			}
		}
		// assert(score == realscore);
	}
#endif

	std::pair<ScoreType, std::vector<MatrixPosition>> getTraceFromTable(const std::string& sequence, const DPTable& slice, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		assert(slice.bandwidthPerSlice.size() == slice.correctness.size());
		assert(sequence.size() % WordConfiguration<Word>::WordSize == 0);
		if (slice.slices.size() == 0)
		{
			return std::make_pair(std::numeric_limits<ScoreType>::max(), std::vector<MatrixPosition>{});
		}
		if (slice.bandwidthPerSlice.size() == 0)
		{
			return std::make_pair(std::numeric_limits<ScoreType>::max(), std::vector<MatrixPosition>{});
		}
		assert(slice.samplingFrequency > 1);
		std::pair<ScoreType, std::vector<MatrixPosition>> result {0, {}};
		size_t backtraceOverrideIndex = -1;
		LengthType lastBacktraceOverrideStartJ = -1;
		LengthType nextBacktraceOverrideEndJ = -1;
		if (slice.backtraceOverrides.size() > 0)
		{
			backtraceOverrideIndex = slice.backtraceOverrides.size()-1;
			nextBacktraceOverrideEndJ = slice.backtraceOverrides.back().endj;
		}
		for (size_t i = slice.slices.size()-1; i < slice.slices.size(); i--)
		{
			if ((slice.slices[i].j + WordConfiguration<Word>::WordSize) / WordConfiguration<Word>::WordSize == slice.bandwidthPerSlice.size())
			{
				assert(i == slice.slices.size() - 1);
				result.first = slice.slices.back().minScore;
				result.second.emplace_back(slice.slices.back().minScoreIndex.back(), slice.slices.back().j + WordConfiguration<Word>::WordSize - 1);
				continue;
			}
			auto partTable = getSlicesFromTable(sequence, lastBacktraceOverrideStartJ, slice, i, nodesliceMap);
			assert(partTable.size() > 0);
			if (i == slice.slices.size() - 1)
			{
				result.first = partTable.back().minScore;
				assert(partTable.back().minScoreIndex.size() > 0);
				result.second.emplace_back(partTable.back().minScoreIndex.back(), partTable.back().j + WordConfiguration<Word>::WordSize - 1);
			}
			auto partTrace = getTraceFromTableInner(sequence, partTable, result.second.back());
			assert(partTrace.size() > 1);
			//begin()+1 because the starting position was already inserted earlier
			result.second.insert(result.second.end(), partTrace.begin()+1, partTrace.end());
			auto boundaryTrace = getSliceBoundaryTrace(sequence, partTable[0], slice.slices[i], result.second.back().first);
			result.second.insert(result.second.end(), boundaryTrace.begin(), boundaryTrace.end());
			assert(boundaryTrace.size() > 0);
			if (slice.slices[i].j == nextBacktraceOverrideEndJ)
			{
				auto trace = slice.backtraceOverrides[backtraceOverrideIndex].GetBacktrace(result.second.back());
				result.second.insert(result.second.end(), trace.begin()+1, trace.end());
				lastBacktraceOverrideStartJ = slice.backtraceOverrides[backtraceOverrideIndex].startj;
				backtraceOverrideIndex--;
				if (backtraceOverrideIndex != -1) nextBacktraceOverrideEndJ = slice.backtraceOverrides[backtraceOverrideIndex].endj;
			}
		}
		assert(result.second.back().second == -1);
		result.second.pop_back();
		assert(result.second.back().second == 0);
		std::reverse(result.second.begin(), result.second.end());
#ifndef NDEBUG
		verifyTrace(result.second, sequence, result.first, slice);
#endif
		return result;
	}

	//returns the trace backwards, aka result[0] is at the bottom of the slice and result.back() at the top
	std::vector<MatrixPosition> getTraceFromSlice(const std::string& sequence, const DPSlice& slice, MatrixPosition pos) const
	{
		assert(pos.second >= slice.j);
		assert(pos.second < slice.j + WordConfiguration<Word>::WordSize);
		// auto distance = params.graph.MinDistance(startColumn, slice.minScoreIndex);
		// std::cerr << "distance from min: " << distance << std::endl;
		// auto score = getValue(slice, WordConfiguration<Word>::WordSize-1, startColumn);
		// std::cerr << "score from min: " << (score - slice.minScore) << std::endl;
		std::vector<MatrixPosition> result;
		while (pos.second != slice.j)
		{
			assert(slice.scores.hasNode(params.graph.IndexToNode(pos.first)));
			pos = pickBacktracePredecessor(params, sequence, slice, pos, slice);
			result.push_back(pos);
		}
		assert(slice.scores.hasNode(params.graph.IndexToNode(pos.first)));
		return result;
	}

	//returns the trace backwards, aka result[0] is after the boundary (later slice) and result.back() over it (earlier slice)
	std::vector<MatrixPosition> getSliceBoundaryTrace(const std::string& sequence, const DPSlice& after, const DPSlice& before, LengthType afterColumn) const
	{
		MatrixPosition pos { afterColumn, after.j };
		assert(after.j == before.j + WordConfiguration<Word>::WordSize);
		std::vector<MatrixPosition> result;
		while (pos.second == after.j)
		{
			assert(after.scores.hasNode(params.graph.IndexToNode(pos.first)));
			pos = pickBacktracePredecessor(params, sequence, after, pos, before);
			result.push_back(pos);
		}
		assert(before.scores.hasNode(params.graph.IndexToNode(pos.first)));
		return result;
	}

	//returns the trace backwards, aka result[0] is at the bottom of the table and result.back() at the top
	std::vector<MatrixPosition> getTraceFromTableInner(const std::string& sequence, const std::vector<DPSlice>& table, MatrixPosition pos) const
	{
		assert(table.size() > 0);
		assert(pos.second >= table.back().j);
		assert(pos.second < table.back().j + WordConfiguration<Word>::WordSize);
		std::vector<MatrixPosition> result;
		result.push_back(pos);
		for (size_t slice = table.size()-1; slice < table.size(); slice--)
		{
			assert(table[slice].j <= result.back().second);
			assert(table[slice].j + WordConfiguration<Word>::WordSize > result.back().second);
			auto partialTrace = getTraceFromSlice(sequence, table[slice], result.back());
			assert(partialTrace.size() >= WordConfiguration<Word>::WordSize - 1);
			result.insert(result.end(), partialTrace.begin(), partialTrace.end());
			assert(result.back().second == table[slice].j);
			if (slice > 0)
			{
				auto boundaryTrace = getSliceBoundaryTrace(sequence, table[slice], table[slice-1], result.back().first);
				result.insert(result.end(), boundaryTrace.begin(), boundaryTrace.end());
				assert(result.back().second == table[slice-1].j + WordConfiguration<Word>::WordSize - 1);
			}
		}
		assert(result.back().second == table[0].j);
		assert(table[0].scores.hasNode(params.graph.IndexToNode(result.back().first)));
		return result;
	}

	class NodePosWithDistance
	{
	public:
		NodePosWithDistance(LengthType node, bool end, int distance) : node(node), end(end), distance(distance) {};
		LengthType node;
		bool end;
		int distance;
		bool operator<(const NodePosWithDistance& other) const
		{
			return distance < other.distance;
		}
		bool operator>(const NodePosWithDistance& other) const
		{
			return distance > other.distance;
		}
	};

	void filterReachableRec(std::set<LengthType>& result, const std::set<LengthType>& current, const std::vector<bool>& previousBand, LengthType start) const
	{
		std::vector<LengthType> stack;
		stack.push_back(start);
		while (stack.size() > 0)
		{
			auto node = stack.back();
			stack.pop_back();
			if (result.count(node) == 1) continue;
			if (current.count(node) == 0) continue;
			result.insert(node);
			for (auto neighbor : params.graph.outNeighbors[node])
			{
				stack.push_back(neighbor);
			}
		}
	}

	std::set<LengthType> filterOnlyReachable(const std::set<LengthType>& nodes, const std::vector<bool>& previousBand) const
	{
		std::set<LengthType> result;
		for (auto node : nodes)
		{
			if (result.count(node) == 1) continue;
			bool inserted = false;
			if (previousBand[node])
			{
				result.insert(node);
				inserted = true;
			}
			else
			{
				for (auto neighbor : params.graph.inNeighbors[node])
				{
					if (previousBand[neighbor])
					{
						result.insert(node);
						inserted = true;
						break;
					}
				}
			}
			if (inserted)
			{
				for (auto neighbor : params.graph.outNeighbors[node])
				{
					filterReachableRec(result, nodes, previousBand, neighbor);
				}
			}
		}

		return result;
	}

	class NodeWithPriority
	{
	public:
		NodeWithPriority(LengthType node, int priority) : node(node), priority(priority) {}
		bool operator>(const NodeWithPriority& other) const
		{
			return priority > other.priority;
		}
		bool operator<(const NodeWithPriority& other) const
		{
			return priority < other.priority;
		}
		LengthType node;
		int priority;
	};

	std::vector<LengthType> projectForwardFromMinScore(ScoreType minScore, const DPSlice& previousSlice, const std::vector<bool>& previousBand, int bandwidth) const
	{
		const auto expandWidth = bandwidth + WordConfiguration<Word>::WordSize;
		std::unordered_map<size_t, size_t> distances;
		std::vector<LengthType> result;
		std::priority_queue<NodeWithPriority, std::vector<NodeWithPriority>, std::greater<NodeWithPriority>> queue;
		size_t currentWidth = 0;
		for (const auto pair : previousSlice.scores)
		{
			if (pair.second.minScore() <= minScore + bandwidth)
			{
				auto node = pair.first;
				distances[node] = 0;
				result.push_back(node);
				currentWidth += params.graph.NodeLength(node);
				if (currentWidth >= params.AlternateMethodCutoff)
				{
					return result;
				}
				auto endscore = pair.second.back().scoreEnd;
				assert(endscore >= minScore);
				if (endscore > minScore + expandWidth) continue;
				for (auto neighbor : params.graph.outNeighbors[node])
				{
					queue.emplace(neighbor, endscore-minScore+1);
				}
			}
		}
		assert(distances.size() > 0);
		while (queue.size() > 0)
		{
			NodeWithPriority top = queue.top();
			if (top.priority > expandWidth) break;
			queue.pop();
			if (distances.count(top.node) == 1 && distances[top.node] <= top.priority) continue;
			currentWidth += params.graph.NodeLength(top.node);
			distances[top.node] = top.priority;
			result.push_back(top.node);
			if (currentWidth >= params.AlternateMethodCutoff)
			{
				return result;
			}
			auto size = params.graph.NodeLength(top.node);
			for (auto neighbor : params.graph.outNeighbors[top.node])
			{
				queue.emplace(neighbor, top.priority + size);
			}
		}
		return result;
	}

#ifdef EXTRABITVECTORASSERTIONS

	WordSlice getWordSliceCellByCell(size_t j, size_t w, const std::string& sequence, const NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand) const
	{
		const auto lastBitMask = ((Word)1) << (WordConfiguration<Word>::WordSize-1);
		WordSlice result;
		auto nodeIndex = params.graph.IndexToNode(w);
		assert(currentBand[nodeIndex]);
		const std::vector<WordSlice>& oldNode = previousBand[nodeIndex] ? previousSlice.node(nodeIndex) : currentSlice.node(nodeIndex);
		assert(currentBand[nodeIndex]);
		ScoreType current[66];
		current[0] = j+1;
		current[1] = j;
		if (j > 0 && previousBand[nodeIndex]) current[1] = std::min(current[1], oldNode[w-params.graph.NodeStart(nodeIndex)].scoreEnd);
		if (j > 0 && previousBand[nodeIndex]) current[0] = std::min(current[0], oldNode[w-params.graph.NodeStart(nodeIndex)].scoreEnd - ((oldNode[w-params.graph.NodeStart(nodeIndex)].VP & lastBitMask) ? 1 : 0) + ((oldNode[w-params.graph.NodeStart(nodeIndex)].VN & lastBitMask) ? 1 : 0));
		for (int i = 1; i < 65; i++)
		{
			current[i+1] = current[i]+1;
		}
		if (w == params.graph.NodeStart(nodeIndex))
		{
			for (auto neighbor : params.graph.inNeighbors[nodeIndex])
			{
				if (!previousBand[neighbor] && !currentBand[neighbor]) continue;
				const std::vector<WordSlice>& neighborSlice = currentBand[neighbor] ? currentSlice.node(neighbor) : previousSlice.node(neighbor);
				const std::vector<WordSlice>& oldNeighborSlice = previousBand[neighbor] ? previousSlice.node(neighbor) : currentSlice.node(neighbor);
				auto u = params.graph.NodeEnd(neighbor)-1;
				ScoreType previous[66];
				previous[0] = j+1;
				previous[1] = j;
				if (j > 0 && previousBand[neighbor]) previous[1] = std::min(previous[1], oldNeighborSlice.back().scoreEnd);
				if (j > 0 && previousBand[neighbor]) previous[0] = std::min(previous[0], oldNeighborSlice.back().scoreEnd - ((oldNeighborSlice.back().VP & lastBitMask) ? 1 : 0) + ((oldNeighborSlice.back().VN & lastBitMask) ? 1 : 0));
				if (currentBand[neighbor]) previous[1] = std::min(previous[1], neighborSlice.back().scoreBeforeStart);
				for (int i = 1; i < 65; i++)
				{
					if (currentBand[neighbor])
					{
						previous[i+1] = previous[i];
						previous[i+1] += (neighborSlice.back().VP & (((Word)1) << (i-1)) ? 1 : 0);
						previous[i+1] -= (neighborSlice.back().VN & (((Word)1) << (i-1)) ? 1 : 0);
					}
					else
					{
						previous[i+1] = previous[i]+1;
					}
				}
				current[0] = std::min(current[0], previous[0]+1);
				for (int i = 0; i < 65; i++)
				{
					current[i+1] = std::min(current[i+1], previous[i+1]+1);
					current[i+1] = std::min(current[i+1], current[i]+1);
					if (j+i > 0 && (sequence[j+i-1] == params.graph.NodeSequences(w) || sequence[j+i-1] == 'N'))
					{
						current[i+1] = std::min(current[i+1], previous[i]);
					}
					else
					{
						current[i+1] = std::min(current[i+1], previous[i]+1);
					}
				}
			}
		}
		else
		{
			const std::vector<WordSlice>& slice = currentSlice.node(nodeIndex);
			const std::vector<WordSlice>& oldSlice = previousBand[nodeIndex] ? previousSlice.node(nodeIndex) : slice;
			auto u = w-1;
			ScoreType previous[66];
			previous[0] = slice[u-params.graph.NodeStart(nodeIndex)].scoreBeforeStart+1;
			previous[1] = slice[u-params.graph.NodeStart(nodeIndex)].scoreBeforeStart;
			if (previousBand[nodeIndex]) previous[0] = std::min(previous[0], oldSlice[u-params.graph.NodeStart(nodeIndex)].scoreEnd - ((oldSlice[u-params.graph.NodeStart(nodeIndex)].VP & lastBitMask) ? 1 : 0) + ((oldSlice[u-params.graph.NodeStart(nodeIndex)].VN & lastBitMask) ? 1 : 0));
			if (previousBand[nodeIndex]) previous[1] = std::min(previous[1], oldSlice[u-params.graph.NodeStart(nodeIndex)].scoreEnd);
			for (int i = 1; i < 65; i++)
			{
				previous[i+1] = previous[i];
				previous[i+1] += (slice[u-params.graph.NodeStart(nodeIndex)].VP & (((Word)1) << (i-1)) ? 1 : 0);
				previous[i+1] -= (slice[u-params.graph.NodeStart(nodeIndex)].VN & (((Word)1) << (i-1)) ? 1 : 0);
			}
			current[0] = std::min(current[0], previous[0]+1);
			for (int i = 0; i < 65; i++)
			{
				current[i+1] = std::min(current[i+1], current[i]+1);
				current[i+1] = std::min(current[i+1], previous[i+1]+1);
				if (j+i > 0 && (sequence[j+i-1] == params.graph.NodeSequences(w) || sequence[j+i-1] == 'N'))
				{
					current[i+1] = std::min(current[i+1], previous[i]);
				}
				else
				{
					current[i+1] = std::min(current[i+1], previous[i]+1);
				}
			}
		}
		for (int i = 1; i < 65; i++)
		{
			assert(current[i+1] >= debugLastRowMinScore);
			assert(current[i+1] >= current[i]-1);
			assert(current[i+1] <= current[i]+1);
			if (current[i+1] == current[i]+1) result.VP |= ((Word)1) << (i-1);
			if (current[i+1] == current[i]-1) result.VN |= ((Word)1) << (i-1);
		}
		result.scoreBeforeStart = current[1];
		result.scoreEnd = current[65];
		assert(result.scoreEnd == result.scoreBeforeStart + WordConfiguration<Word>::popcount(result.VP) - WordConfiguration<Word>::popcount(result.VN));
		return result;
	}

#endif

	WordSlice getNodeStartSlice(const Word Eq, const size_t nodeIndex, const NodeSlice<WordSlice>& previousSlice, const NodeSlice<WordSlice>& currentSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, const bool previousEq) const
	{
		const WordSlice current = currentSlice.node(nodeIndex)[0];
		WordSlice result;
		bool foundOne = false;
		for (auto neighbor : params.graph.inNeighbors[nodeIndex])
		{
			if (!currentBand[neighbor] && !previousBand[neighbor]) continue;
			Word EqHere = Eq;
			WordSlice previous;
			WordSlice previousUp;
			bool foundOneUp = false;
			bool hasRealNeighbor = false;
			if (currentBand[neighbor] && previousBand[neighbor]) assertSliceCorrectness(currentSlice.node(neighbor).back(), previousSlice.node(neighbor).back(), previousBand[neighbor]);
			if (previousBand[neighbor])
			{
				previousUp = previousSlice.node(neighbor).back();
				foundOneUp = true;
			}
			if (currentBand[neighbor])
			{
				previous = currentSlice.node(neighbor).back();
				hasRealNeighbor = true;
			}
			else
			{
				previous = getSourceSliceFromScore(previousSlice.node(neighbor).back().scoreEnd);
				assert(previousBand[neighbor]);
				previous.scoreBeforeExists = true;
			}
			assertSliceCorrectness(previous, previousUp, foundOneUp);
			if (!hasRealNeighbor) EqHere &= 1;
			auto resultHere = getNextSlice(EqHere, previous, current.scoreBeforeExists, current.scoreBeforeExists && foundOneUp, foundOneUp, previousEq, previousUp);
			if (!foundOne)
			{
				result = resultHere;
				foundOne = true;
			}
			else
			{
				result = result.mergeWith(resultHere);
			}
		}
		assert(foundOne);
		return result;
	}

	WordSlice getSourceSliceWithoutBefore(size_t row) const
	{
		return { WordConfiguration<Word>::AllOnes & ~((Word)1), WordConfiguration<Word>::AllZeros, row+WordConfiguration<Word>::WordSize, row+1, WordConfiguration<Word>::WordSize, false };
	}

	WordSlice getSourceSliceFromScore(ScoreType previousScore) const
	{
		return { WordConfiguration<Word>::AllOnes, WordConfiguration<Word>::AllZeros, previousScore+WordConfiguration<Word>::WordSize, previousScore, WordConfiguration<Word>::WordSize, false };
	}

	WordSlice getSourceSliceFromStartMatch(char sequenceChar, char graphChar, ScoreType previousScore) const
	{
		Word firstVP = characterMatch(sequenceChar, graphChar) ? 0 : 1;
		return { WordConfiguration<Word>::AllOnes & ~(Word)1 | firstVP, WordConfiguration<Word>::AllZeros, previousScore+WordConfiguration<Word>::WordSize - 1 + firstVP, previousScore, WordConfiguration<Word>::WordSize, true };
	}

	WordSlice getSourceSliceFromBefore(size_t nodeIndex, const NodeSlice<WordSlice>& previousSlice) const
	{
		auto previousWordSlice = previousSlice.node(nodeIndex)[0];
		return { WordConfiguration<Word>::AllOnes, WordConfiguration<Word>::AllZeros, previousWordSlice.scoreEnd+WordConfiguration<Word>::WordSize, previousWordSlice.scoreEnd, WordConfiguration<Word>::WordSize, previousWordSlice.scoreEndExists };
	}

	bool isSource(size_t nodeIndex, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand) const
	{
		for (auto neighbor : params.graph.inNeighbors[nodeIndex])
		{
			if (currentBand[neighbor]) return false;
			if (previousBand[neighbor]) return false;
		}
		return true;
	}

	WordSlice getNextSlice(Word Eq, WordSlice slice, bool upInsideBand, bool upleftInsideBand, bool diagonalInsideBand, bool previousEq, WordSlice previous) const
	{
		//http://www.gersteinlab.org/courses/452/09-spring/pdf/Myers.pdf
		//pages 405 and 408

		auto oldValue = slice.scoreBeforeStart;
		Word confirmedMask = ((Word)1) << slice.confirmedRows.rows;
		Word prevConfirmedMask = ((Word)1) << (slice.confirmedRows.rows - 1);
		bool confirmOneMore = false;
		if (!slice.scoreBeforeExists) Eq &= ~((Word)1);
		slice.scoreBeforeExists = upInsideBand;
		if (!diagonalInsideBand) Eq &= ~((Word)1);
		if (!upleftInsideBand)
		{
			slice.scoreBeforeStart += 1;
		}
		else
		{
			const auto lastBitMask = ((Word)1) << (WordConfiguration<Word>::WordSize - 1);
			assert(slice.scoreBeforeStart <= previous.scoreEnd);
			slice.scoreBeforeStart = std::min(slice.scoreBeforeStart + 1, previous.scoreEnd - ((previous.VP & lastBitMask) ? 1 : 0) + ((previous.VN & lastBitMask) ? 1 : 0) + (previousEq ? 0 : 1));
		}
		auto hin = slice.scoreBeforeStart - oldValue;

		Word Xv = Eq | slice.VN;
		//between 7 and 8
		if (hin < 0) Eq |= 1;
		Word Xh = (((Eq & slice.VP) + slice.VP) ^ slice.VP) | Eq;
		Word Ph = slice.VN | ~(Xh | slice.VP);
		Word Mh = slice.VP & Xh;
		int diagonalDiff = hin;
		if (slice.confirmedRows.rows > 0) diagonalDiff = ((Ph & prevConfirmedMask) ? 1 : 0) - ((Mh & prevConfirmedMask) ? 1 : 0);
		if (slice.confirmedRows.rows > 0 && (Mh & prevConfirmedMask)) confirmOneMore = true;
		else if (slice.confirmedRows.rows == 0 && hin == -1) confirmOneMore = true;
		// if (~Ph & confirmedMask) confirmTentative = true;
		const Word lastBitMask = (((Word)1) << (WordConfiguration<Word>::WordSize - 1));
		if (Ph & lastBitMask)
		{
			slice.scoreEnd += 1;
		}
		else if (Mh & lastBitMask)
		{
			slice.scoreEnd -= 1;
		}
		if (slice.confirmedRows.partial && (~Ph & confirmedMask)) confirmOneMore = true;
		Ph <<= 1;
		Mh <<= 1;
		//between 16 and 17
		if (hin < 0) Mh |= 1; else if (hin > 0) Ph |= 1;
		slice.VP = Mh | ~(Xv | Ph);
		slice.VN = Ph & Xv;
		diagonalDiff += ((slice.VP & confirmedMask) ? 1 : 0) - ((slice.VN & confirmedMask) ? 1 : 0);
		if (diagonalDiff <= 0) confirmOneMore = true;
		else if (slice.VN & confirmedMask) confirmOneMore = true;

		if (confirmOneMore)
		{
			//somehow std::min(slice.confirmedRows+1, WordConfiguration<Word>::WordSize) doesn't work here?!
			if (slice.confirmedRows.rows + 1 <= WordConfiguration<Word>::WordSize)
			{
				slice.confirmedRows.rows += 1;
			}
			slice.confirmedRows.partial = false;
		}
		else if (!slice.confirmedRows.partial && slice.confirmedRows.rows < WordConfiguration<Word>::WordSize)
		{
			slice.confirmedRows.partial = true;
		}

#ifndef NDEBUG
		auto wcvp = WordConfiguration<Word>::popcount(slice.VP);
		auto wcvn = WordConfiguration<Word>::popcount(slice.VN);
		assert(slice.scoreEnd == slice.scoreBeforeStart + wcvp - wcvn);
		assert(slice.confirmedRows.rows < WordConfiguration<Word>::WordSize || slice.scoreBeforeStart >= debugLastRowMinScore);
		assert(slice.confirmedRows.rows < WordConfiguration<Word>::WordSize || slice.scoreEnd >= debugLastRowMinScore);
#endif

		return slice;
	}

	class NodeCalculationResult
	{
	public:
		ScoreType minScore;
		std::vector<LengthType> minScoreIndex;
		size_t cellsProcessed;
	};

	void assertSliceCorrectness(const WordSlice& current, const WordSlice& up, bool previousBand) const
	{
#ifndef NDEBUG
		auto wcvp = WordConfiguration<Word>::popcount(current.VP);
		auto wcvn = WordConfiguration<Word>::popcount(current.VN);
		assert(current.scoreEnd == current.scoreBeforeStart + wcvp - wcvn);

		assert(current.scoreBeforeStart >= 0);
		assert(current.scoreEnd >= 0);
		assert(current.scoreBeforeStart <= current.scoreEnd + WordConfiguration<Word>::WordSize);
		assert(current.scoreEnd <= current.scoreBeforeStart + WordConfiguration<Word>::WordSize);
		assert((current.VP & current.VN) == WordConfiguration<Word>::AllZeros);

		assert(!previousBand || current.scoreBeforeStart <= up.scoreEnd);
		assert(current.scoreBeforeStart >= 0);
		assert(current.confirmedRows.rows < WordConfiguration<Word>::WordSize || current.scoreEnd >= debugLastRowMinScore);
		assert(current.confirmedRows.rows < WordConfiguration<Word>::WordSize || current.scoreBeforeStart >= debugLastRowMinScore);
#endif
	}

	NodeCalculationResult calculateNode(size_t i, size_t j, const std::string& sequence, const EqVector& EqV, NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand) const
	{
		NodeCalculationResult result;
		result.minScore = std::numeric_limits<ScoreType>::max();
		result.cellsProcessed = 0;
		auto slice = currentSlice.node(i);
		const auto oldSlice = previousBand[i] ? previousSlice.node(i) : slice;
		assert(slice.size() == params.graph.NodeEnd(i) - params.graph.NodeStart(i));
		auto nodeStart = params.graph.NodeStart(i);

#ifdef EXTRABITVECTORASSERTIONS
		WordSlice correctstart;
		correctstart = getWordSliceCellByCell(j, params.graph.NodeStart(i), sequence, currentSlice, previousSlice, currentBand, previousBand);
#endif

		auto oldConfirmation = slice[0].confirmedRows;
		if (oldConfirmation.rows == WordConfiguration<Word>::WordSize) return result;

		if (isSource(i, currentBand, previousBand))
		{
			if (j == 0 && previousBand[i])
			{
				slice[0] = getSourceSliceFromStartMatch(sequence[0], params.graph.NodeSequences(nodeStart), previousSlice.node(i)[0].scoreEnd);
			}
			else if (previousBand[i])
			{
				slice[0] = getSourceSliceFromBefore(i, previousSlice);
			}
			else
			{
				slice[0] = getSourceSliceWithoutBefore(sequence.size());
			}
			if (slice[0].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[0].scoreEnd < result.minScore)
			{
				result.minScore = slice[0].scoreEnd;
				result.minScoreIndex.clear();
			}
			if (slice[0].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[0].scoreEnd == result.minScore)
			{
				result.minScoreIndex.push_back(nodeStart);
			}
			assertSliceCorrectness(slice[0], oldSlice[0], previousBand[i]);
		}
		else
		{
			Word Eq = EqV.getEq(params.graph.NodeSequences(nodeStart));
			slice[0] = getNodeStartSlice(Eq, i, previousSlice, currentSlice, currentBand, previousBand, (j == 0 && previousBand[i]) || (j > 0 && params.graph.NodeSequences(params.graph.NodeStart(i)) == sequence[j-1]));
			if (previousBand[i] && slice[0].scoreBeforeStart > oldSlice[0].scoreEnd)
			{
				auto mergable = getSourceSliceFromScore(oldSlice[0].scoreEnd);
				mergable.scoreBeforeExists = oldSlice[0].scoreEndExists;
				slice[0] = slice[0].mergeWith(mergable);
			}
			if (slice[0].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[0].scoreEnd < result.minScore)
			{
				result.minScore = slice[0].scoreEnd;
				result.minScoreIndex.clear();
			}
			if (slice[0].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[0].scoreEnd == result.minScore)
			{
				result.minScoreIndex.push_back(nodeStart);
			}
			assertSliceCorrectness(slice[0], oldSlice[0], previousBand[i]);
			//note: currentSlice[start].score - optimalInNeighborEndScore IS NOT within {-1, 0, 1} always because of the band
		}

		assert(slice[0].confirmedRows >= oldConfirmation);
		if (slice[0].confirmedRows == oldConfirmation) return result;

#ifdef EXTRABITVECTORASSERTIONS
		assert(slice[0].scoreBeforeStart == correctstart.scoreBeforeStart);
		assert(slice[0].scoreEnd == correctstart.scoreEnd);
		assert(slice[0].VP == correctstart.VP);
		assert(slice[0].VN == correctstart.VN);
#endif

		for (LengthType w = 1; w < params.graph.NodeEnd(i) - params.graph.NodeStart(i); w++)
		{
			Word Eq = EqV.getEq(params.graph.NodeSequences(nodeStart+w));

			oldConfirmation = slice[w].confirmedRows;
			if (oldConfirmation.rows == WordConfiguration<Word>::WordSize) return result;

			slice[w] = getNextSlice(Eq, slice[w-1], slice[w].scoreBeforeExists, slice[w].scoreBeforeExists, slice[w-1].scoreBeforeExists, (j == 0 && previousBand[i]) || (j > 0 && params.graph.NodeSequences(nodeStart+w) == sequence[j-1]), oldSlice[w-1]);
			if (previousBand[i] && slice[w].scoreBeforeStart > oldSlice[w].scoreEnd)
			{
				auto mergable = getSourceSliceFromScore(oldSlice[w].scoreEnd);
				mergable.scoreBeforeExists = oldSlice[w].scoreEndExists;
				slice[w] = slice[w].mergeWith(mergable);
			}

			assert(previousBand[i] || slice[w].scoreBeforeStart == j || slice[w].scoreBeforeStart == slice[w-1].scoreBeforeStart + 1);
			assertSliceCorrectness(slice[w], oldSlice[w], previousBand[i]);

			if (slice[w].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[w].scoreEnd < result.minScore)
			{
				result.minScore = slice[w].scoreEnd;
				result.minScoreIndex.clear();
			}
			if (slice[w].confirmedRows.rows == WordConfiguration<Word>::WordSize && slice[w].scoreEnd == result.minScore)
			{
				result.minScoreIndex.push_back(nodeStart + w);
			}

			if (slice[w].confirmedRows == oldConfirmation) return result;

#ifdef EXTRABITVECTORASSERTIONS
			auto correctslice = getWordSliceCellByCell(j, nodeStart+w, sequence, currentSlice, previousSlice, currentBand, previousBand);
			assert(slice[w].scoreBeforeStart == correctslice.scoreBeforeStart);
			assert(slice[w].scoreEnd == correctslice.scoreEnd);
			assert(slice[w].VP == correctslice.VP);
			assert(slice[w].VN == correctslice.VN);
#endif
		}
		result.cellsProcessed = (params.graph.NodeEnd(i) - params.graph.NodeStart(i)) * WordConfiguration<Word>::WordSize;
		return result;
	}

	std::vector<LengthType> forwardFromMinScoreBandFunction(const std::vector<bool>& previousBand, const DPSlice& previousSlice, int bandwidth) const
	{
		return projectForwardFromMinScore(previousSlice.minScore, previousSlice, previousBand, bandwidth);
	}

	std::vector<LengthType> rowBandFunction(const DPSlice& previousSlice, const std::vector<bool>& previousBand, int bandwidth) const
	{
		return forwardFromMinScoreBandFunction(previousBand, previousSlice, bandwidth);
	}

#ifdef EXTRACORRECTNESSASSERTIONS

	ScoreType getValueIfExists(const DPSlice& slice, int row, LengthType cell, ScoreType defaultValue) const
	{
		auto nodeIndex = params.graph.IndexToNode(cell);
		if (!slice.scores.hasNode(nodeIndex)) return defaultValue;
		auto wordslice = slice.scores.node(nodeIndex)[cell - params.graph.NodeStart(nodeIndex)];
		return wordslice.getValueIfExists(row);
	}

	template <typename T>
	T volmin(volatile T& a, T b) const
	{
		return std::min((T)a, b);
	}

	void verifySliceBitvector(const std::string& sequence, const DPSlice& current, const DPSlice& previous) const
	{
		const ScoreType uninitScore = sequence.size() + 10000;
		const auto lastrow = WordConfiguration<Word>::WordSize - 1;
		for (auto pair : current.scores)
		{
			auto start = params.graph.NodeStart(pair.first);
			for (size_t i = 1; i < pair.second.size(); i++)
			{
				volatile bool match = characterMatch(sequence[current.j], params.graph.NodeSequences(start+i));
				volatile ScoreType foundMinScore = uninitScore;
				foundMinScore = volmin(foundMinScore, getValue(params.graph, current, 0, start+i-1)+1);
				if (previous.scores.hasNode(pair.first))
				{
					foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, start+i)+1);
					if (previous.scores.node(pair.first)[i-1].scoreEndExists)
					{
						foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, start+i-1) + (match ? 0 : 1));
					}
					else
					{
						foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, start+i-1) + 1);
					}
				}
				assert(getValue(params.graph, current, 0, start+i) == foundMinScore);
				for (int j = 1; j < WordConfiguration<Word>::WordSize; j++)
				{
					match = characterMatch(sequence[current.j+j], params.graph.NodeSequences(start+i));
					foundMinScore = uninitScore;
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j-1, start+i)+1);
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j, start+i-1)+1);
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j-1, start+i-1)+(match ? 0 : 1));
					assert(getValue(params.graph, current, j, start+i) == foundMinScore);
				}
			}
			volatile ScoreType foundMinScore = uninitScore;
			volatile bool match = characterMatch(sequence[current.j], params.graph.NodeSequences(start));
			if (current.j == 0 && previous.scores.hasNode(pair.first))
			{
				foundMinScore = volmin(foundMinScore, match ? 0 : 1);
			}
			if (previous.scores.hasNode(pair.first))
			{
				foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, start)+1);
			}
			for (auto neighbor : params.graph.inNeighbors[pair.first])
			{
				if (current.scores.hasNode(neighbor))
				{
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, 0, params.graph.NodeEnd(neighbor)-1)+1);
				}
				if (previous.scores.hasNode(neighbor))
				{
					if (previous.scores.node(neighbor).back().scoreEndExists)
					{
						foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, params.graph.NodeEnd(neighbor)-1) + (match ? 0 : 1));
					}
					else
					{
						foundMinScore = volmin(foundMinScore, getValue(params.graph, previous, lastrow, params.graph.NodeEnd(neighbor)-1)+1);
					}
				}
			}
			assert(getValue(params.graph, current, 0, start) == foundMinScore);
			for (int j = 1; j < WordConfiguration<Word>::WordSize; j++)
			{
				foundMinScore = uninitScore;
				match = characterMatch(sequence[current.j+j], params.graph.NodeSequences(start));
				foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j-1, start)+1);
				for (auto neighbor : params.graph.inNeighbors[pair.first])
				{
					if (!current.scores.hasNode(neighbor)) continue;
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j, params.graph.NodeEnd(neighbor)-1)+1);
					foundMinScore = volmin(foundMinScore, getValue(params.graph, current, j-1, params.graph.NodeEnd(neighbor)-1)+(match ? 0 : 1));
				}
				assert(getValue(params.graph, current, j, start) == foundMinScore);
			}
		}
	}

	void verifySliceAlternate(const std::string& sequence, const DPSlice& current, const DPSlice& previous, bool includeAll, int bandwidth) const
	{
		for (auto pair : current.scores)
		{
			auto start = params.graph.NodeStart(pair.first);
			auto uninitializedValue = pair.second.minScore() + pair.second.size() + bandwidth + 1;
			for (size_t i = 1; i < pair.second.size(); i++)
			{
				volatile bool match = characterMatch(sequence[current.j], params.graph.NodeSequences(start+i));
				volatile ScoreType foundMinScore = sequence.size();
				volatile ScoreType vertscore = getValueIfExists(previous, WordConfiguration<Word>::WordSize-1, start+i, sequence.size());
				volatile ScoreType diagscore = getValueIfExists(previous, WordConfiguration<Word>::WordSize-1, start+i-1, sequence.size());
				volatile ScoreType horiscore = getValueIfExists(current, 0, start+i-1, sequence.size());
				if (cellExists(current, 0, start+i))
				{
					assert(getValue(params.graph, current, 0, start+i) == std::min(std::min(vertscore + 1, horiscore + 1), diagscore + (match ? 0 : 1)));
				}
				for (size_t j = 1; j < WordConfiguration<Word>::WordSize; j++)
				{
					match = characterMatch(sequence[current.j+j], params.graph.NodeSequences(start+i));
					vertscore = getValueIfExists(current, j-1, start+i, sequence.size());
					horiscore = getValueIfExists(current, j, start+i-1, sequence.size());
					diagscore = getValueIfExists(current, j-1, start+i-1, sequence.size());
					if (cellExists(current, j, start+i))
					{
						assert(getValue(params.graph, current, j, start+i) == std::min(std::min(vertscore + 1, horiscore + 1), diagscore + (match ? 0 : 1)));
					}
				}
			}
			volatile ScoreType vertscore = sequence.size();
			volatile ScoreType diagscore = sequence.size();
			volatile ScoreType horiscore = sequence.size();
			if (previous.scores.hasNode(pair.first)) vertscore = getValue(params.graph, previous, WordConfiguration<Word>::WordSize-1, start);
			for (auto neighbor : params.graph.inNeighbors[pair.first])
			{
				horiscore = volmin(horiscore, getValueIfExists(current, 0, params.graph.NodeEnd(neighbor)-1, sequence.size()));
				diagscore = volmin(diagscore, getValueIfExists(previous, WordConfiguration<Word>::WordSize-1, params.graph.NodeEnd(neighbor)-1, sequence.size()));
			}
			volatile bool match = characterMatch(sequence[current.j], params.graph.NodeSequences(start));
			if (current.j == 0 && previous.scores.hasNode(pair.first))
			{
				assert(getValue(params.graph, current, 0, start) == match ? 0 : 1);
			}
			else if (cellExists(current, 0, start))
			{
				assert(getValue(params.graph, current, 0, start) == std::min(std::min(vertscore + 1, horiscore + 1), diagscore + (match ? 0 : 1)));
			}
			for (size_t j = 1; j < WordConfiguration<Word>::WordSize; j++)
			{
				vertscore = getValueIfExists(current, j-1, start, sequence.size());
				horiscore = sequence.size();
				diagscore = sequence.size();
				for (auto neighbor : params.graph.inNeighbors[pair.first])
				{
					horiscore = volmin(horiscore, getValueIfExists(current, j, params.graph.NodeEnd(neighbor)-1, sequence.size()));
					diagscore = volmin(diagscore, getValueIfExists(current, j-1, params.graph.NodeEnd(neighbor)-1, sequence.size()));
				}
				match = characterMatch(sequence[current.j+j], params.graph.NodeSequences(start));
				if (cellExists(current, j, start))
				{
					assert(getValue(params.graph, current, j, start) == std::min(std::min(vertscore + 1, horiscore + 1), diagscore + (match ? 0 : 1)));
				}
			}
		}
	}

#endif

	//https://stackoverflow.com/questions/159590/way-to-go-from-recursion-to-iteration
	//https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
	class ComponentAlgorithmCallStack
	{
	public:
		ComponentAlgorithmCallStack(LengthType nodeIndex, int state) : nodeIndex(nodeIndex), state(state) {}
		LengthType nodeIndex;
		int state;
		std::vector<size_t>::const_iterator neighborIterator;
	};
	void getStronglyConnectedComponentsRec(LengthType start, const std::vector<bool>& currentBand, std::unordered_map<LengthType, size_t>& index, std::unordered_map<LengthType, size_t>& lowLink, size_t& stackindex, std::unordered_set<LengthType>& onStack, std::vector<LengthType>& stack, std::vector<std::vector<LengthType>>& result) const
	{
		assert(currentBand[start]);
		std::vector<ComponentAlgorithmCallStack> callStack;
		callStack.emplace_back(start, 0);
		while (callStack.size() > 0)
		{
			LengthType nodeIndex = callStack.back().nodeIndex;
			int state = callStack.back().state;
			auto iterator = callStack.back().neighborIterator;
			size_t neighbor;
			callStack.pop_back();
			switch(state)
			{
			case 0:
				assert(index.count(nodeIndex) == 0);
				assert(lowLink.count(nodeIndex) == 0);
				assert(onStack.count(nodeIndex) == 0);
				index[nodeIndex] = stackindex;
				lowLink[nodeIndex] = stackindex;
				stackindex++;
				stack.push_back(nodeIndex);
				onStack.insert(nodeIndex);
				iterator = params.graph.outNeighbors[nodeIndex].begin();
			startloop:
				//all neighbors processed
				if (iterator == params.graph.outNeighbors[nodeIndex].end()) goto end;
				neighbor = *iterator;
				//neighbor not in the subgraph, go to next
				if (!currentBand[neighbor])
				{
					++iterator;
					goto startloop;
				}
				//recursive call
				if (index.count(neighbor) == 0)
				{
					callStack.emplace_back(nodeIndex, 1);
					callStack.back().neighborIterator = iterator;
					callStack.emplace_back(neighbor, 0);
					continue;
				}
				if (onStack.count(neighbor) == 1)
				{
					assert(index.count(neighbor) == 1);
					lowLink[nodeIndex] = std::min(lowLink[nodeIndex], index[neighbor]);
				}
				++iterator;
				goto startloop;
			case 1:
				//handle the results of the recursive call
				neighbor = *iterator;
				assert(lowLink.count(neighbor) == 1);
				lowLink[nodeIndex] = std::min(lowLink[nodeIndex], lowLink[neighbor]);
				//next neighbor
				++iterator;
				goto startloop;
			end:
				if (lowLink[nodeIndex] == index[nodeIndex])
				{
					result.emplace_back();
					assert(stack.size() > 0);
					auto back = stack.back();
					do
					{
						back = stack.back();
						result.back().emplace_back(back);
						onStack.erase(back);
						stack.pop_back();
					} while (back != nodeIndex);
					result.back().shrink_to_fit();
				}
			}
		}
	}

	//https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
	std::vector<std::vector<LengthType>> getStronglyConnectedComponents(const std::vector<size_t>& nodes, const std::vector<bool>& currentBand) const
	{
		std::vector<std::vector<LengthType>> result;
		std::unordered_map<LengthType, size_t> index;
		std::unordered_map<LengthType, size_t> lowLink;
		size_t stackIndex = 0;
		std::unordered_set<size_t> onStack;
		std::vector<size_t> stack;
		stack.reserve(nodes.size());
		index.reserve(nodes.size());
		lowLink.reserve(nodes.size());
		onStack.reserve(nodes.size());
		for (auto node : nodes)
		{
			assert(currentBand[node]);
			if (index.count(node) == 0)
			{
				getStronglyConnectedComponentsRec(node, currentBand, index, lowLink, stackIndex, onStack, stack, result);
			}
		}
		result.shrink_to_fit();
		assert(stack.size() == 0);
		assert(onStack.size() == 0);
		assert(index.size() == nodes.size());
		assert(lowLink.size() == nodes.size());
#ifndef NDEBUG
		std::unordered_set<size_t> debugFoundNodes;
		std::unordered_map<size_t, size_t> debugComponentIndex;
		size_t debugComponenti = 0;
		for (const auto& component : result)
		{
			for (auto node : component)
			{
				assert(debugComponentIndex.count(node) == 0);
				debugComponentIndex[node] = debugComponenti;
				assert(debugFoundNodes.count(node) == 0);
				debugFoundNodes.insert(node);
			}
			debugComponenti += 1;
		}
		for (auto node : nodes)
		{
			assert(debugFoundNodes.count(node) == 1);
		}
		for (const auto& component : result)
		{
			for (auto node : component)
			{
				for (auto neighbor : params.graph.outNeighbors[node])
				{
					if (!currentBand[neighbor]) continue;
					assert(debugComponentIndex.count(neighbor) == 1);
					assert(debugComponentIndex[neighbor] <= debugComponenti);
				}
			}
		}
		assert(debugFoundNodes.size() == nodes.size());
		size_t debugTotalNodes = 0;
		for (const auto& component : result)
		{
			debugTotalNodes += component.size();
		}
		assert(debugTotalNodes == nodes.size());
#endif
		return result;
	}

	void forceComponentZeroRow(NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, const std::vector<LengthType>& component, size_t componentIndex, const std::vector<size_t>& partOfComponent, size_t sequenceLen) const
	{
		std::priority_queue<NodeWithPriority, std::vector<NodeWithPriority>, std::greater<NodeWithPriority>> queue;
		for (auto node : component)
		{
			assert(currentBand[node]);
			assert(partOfComponent[node] == componentIndex);
			const auto oldSlice = previousBand[node] ? previousSlice.node(node) : currentSlice.node(node);
			auto newSlice = currentSlice.node(node);
			for (size_t i = 0; i < newSlice.size(); i++)
			{
				newSlice[i].scoreBeforeStart = std::numeric_limits<ScoreType>::max();
			}
			if (previousBand[node])
			{
				newSlice[0].scoreBeforeStart = oldSlice[0].scoreEnd;
			}
			for (auto neighbor : params.graph.inNeighbors[node])
			{
				if (!currentBand[neighbor] && !previousBand[neighbor]) continue;
				if (partOfComponent[neighbor] == componentIndex) continue;
				if (currentBand[neighbor])
				{
					assert(partOfComponent[neighbor] != std::numeric_limits<size_t>::max());
					assert(currentSlice.hasNode(neighbor));
					assert(currentSlice.node(neighbor).back().confirmedRows.rows == WordConfiguration<Word>::WordSize);
					newSlice[0].scoreBeforeStart = std::min(newSlice[0].scoreBeforeStart, currentSlice.node(neighbor).back().scoreBeforeStart + 1);
				}
				if (previousBand[neighbor])
				{
					assert(previousSlice.hasNode(neighbor));
					assert(previousSlice.node(neighbor).back().confirmedRows.rows == WordConfiguration<Word>::WordSize);
					newSlice[0].scoreBeforeStart = std::min(newSlice[0].scoreBeforeStart, previousSlice.node(neighbor).back().scoreEnd + 1);
				}
			}
			if (newSlice[0].scoreBeforeStart == std::numeric_limits<ScoreType>::max()) continue;
			for (size_t i = 1; i < newSlice.size(); i++)
			{
				assert(newSlice[i-1].scoreBeforeStart != std::numeric_limits<ScoreType>::max());
				newSlice[i].scoreBeforeStart = newSlice[i-1].scoreBeforeStart+1;
				if (previousBand[node]) newSlice[i].scoreBeforeStart = std::min(newSlice[i].scoreBeforeStart, oldSlice[i].scoreEnd);
			}
			for (auto neighbor : params.graph.outNeighbors[node])
			{
				if (partOfComponent[neighbor] != componentIndex) continue;
				assert(newSlice.back().scoreBeforeStart != std::numeric_limits<ScoreType>::max());
				queue.emplace(neighbor, newSlice.back().scoreBeforeStart+1);
			}
		}
		while (queue.size() > 0)
		{
			auto top = queue.top();
			queue.pop();
			auto nodeIndex = top.node;
			auto score = top.priority;
			assert(partOfComponent[nodeIndex] == componentIndex);
			bool endUpdated = true;
			auto slice = currentSlice.node(nodeIndex);
			for (size_t i = 0; i < slice.size(); i++)
			{
				if (slice[i].scoreBeforeStart <= score)
				{
					endUpdated = false;
					break;
				}
				assert(slice[i].scoreBeforeStart > score);
				slice[i].scoreBeforeStart = score;
				score++;
			}
			if (endUpdated)
			{
				for (auto neighbor : params.graph.outNeighbors[nodeIndex])
				{
					if (partOfComponent[neighbor] != componentIndex) continue;
					queue.emplace(neighbor, score);
				}
			}
		}
		for (auto node : component)
		{
			assert(currentSlice.hasNode(node));
			auto slice = currentSlice.node(node);
			const auto oldSlice = previousBand[node] ? previousSlice.node(node) : currentSlice.node(node);
			for (size_t i = 0; i < slice.size(); i++)
			{
				assert(slice[i].scoreBeforeStart != std::numeric_limits<ScoreType>::max());
				slice[i] = {WordConfiguration<Word>::AllOnes, 0, slice[i].scoreBeforeStart+WordConfiguration<Word>::WordSize, slice[i].scoreBeforeStart, 0, previousBand[node] && oldSlice[i].scoreEnd == slice[i].scoreBeforeStart && oldSlice[i].scoreEndExists };
#ifdef EXTRACORRECTNESSASSERTIONS
				slice[i].confirmedRows.exists = -1;
#endif
			}
		}
	}

	static ScoreType getValueOrMax(const Params& params, const DPTable& band, LengthType j, LengthType w, ScoreType max)
	{
		auto node = params.graph.IndexToNode(w);
		auto slice = (j - band.startj) / WordConfiguration<Word>::WordSize / band.samplingFrequency;
		assert(band.slices[slice].j == (j / WordConfiguration<Word>::WordSize) * WordConfiguration<Word>::WordSize);
		if (!band.slices[slice].scores.hasNode(node)) return max;
		auto word = band.slices[slice].scores.node(node)[w - params.graph.NodeStart(node)];
		auto off = j % WordConfiguration<Word>::WordSize;
		return word.getValue(off);
	}

	static ScoreType getValueOrMax(const Params& params, const DPSlice& slice, LengthType j, LengthType w, ScoreType max)
	{
		assert(j >= 0);
		assert(j < WordConfiguration<Word>::WordSize);
		auto node = params.graph.IndexToNode(w);
		if (!slice.scores.hasNode(node)) return max;
		auto word = slice.scores.node(node)[w - params.graph.NodeStart(node)];
		auto off = j % WordConfiguration<Word>::WordSize;
		return word.getValue(off);
	}

	static ScoreType getValue(const Params& params, const DPSlice& slice, LengthType j, LengthType w)
	{
		assert(j >= 0);
		assert(j < WordConfiguration<Word>::WordSize);
		auto node = params.graph.IndexToNode(w);
		auto word = slice.scores.node(node)[w - params.graph.NodeStart(node)];
		auto off = j % WordConfiguration<Word>::WordSize;
		return word.getValue(off);
	}

	static ScoreType getValue(const Params& params, const DPTable& band, LengthType j, LengthType w)
	{
		auto node = params.graph.IndexToNode(w);
		auto slice = (j - band.startj) / WordConfiguration<Word>::WordSize / band.samplingFrequency;
		assert(band.slices[slice].j == (j / WordConfiguration<Word>::WordSize) * WordConfiguration<Word>::WordSize);
		auto word = band.slices[slice].scores.node(node)[w - params.graph.NodeStart(node)];
		auto off = j % WordConfiguration<Word>::WordSize;
		return word.getValue(off);
	}

	static bool characterMatch(char sequenceCharacter, char graphCharacter)
	{
		assert(graphCharacter == 'A' || graphCharacter == 'T' || graphCharacter == 'C' || graphCharacter == 'G');
		switch(sequenceCharacter)
		{
			case 'A':
			case 'a':
			return graphCharacter == 'A';
			break;
			case 'T':
			case 't':
			return graphCharacter == 'T';
			break;
			case 'C':
			case 'c':
			return graphCharacter == 'C';
			break;
			case 'G':
			case 'g':
			return graphCharacter == 'G';
			break;
			case 'N':
			case 'n':
			return true;
			break;
			case 'R':
			case 'r':
			return graphCharacter == 'A' || graphCharacter == 'G';
			break;
			case 'Y':
			case 'y':
			return graphCharacter == 'C' || graphCharacter == 'T';
			break;
			case 'K':
			case 'k':
			return graphCharacter == 'G' || graphCharacter == 'T';
			break;
			case 'M':
			case 'm':
			return graphCharacter == 'C' || graphCharacter == 'A';
			break;
			case 'S':
			case 's':
			return graphCharacter == 'C' || graphCharacter == 'G';
			break;
			case 'W':
			case 'w':
			return graphCharacter == 'A' || graphCharacter == 'T';
			break;
			case 'B':
			case 'b':
			return graphCharacter == 'C' || graphCharacter == 'G' || graphCharacter == 'T';
			break;
			case 'D':
			case 'd':
			return graphCharacter == 'A' || graphCharacter == 'G' || graphCharacter == 'T';
			break;
			case 'H':
			case 'h':
			return graphCharacter == 'A' || graphCharacter == 'C' || graphCharacter == 'T';
			break;
			case 'V':
			case 'v':
			return graphCharacter == 'A' || graphCharacter == 'C' || graphCharacter == 'G';
			break;
			default:
			assert(false);
			std::abort();
			return false;
			break;
		}
	}

#ifdef EXTRACORRECTNESSASSERTIONS
	void assertBitvectorConfirmedAreConsistent(WordSlice left, WordSlice right) const
	{
		assert(left.scoreBeforeStart == right.scoreBeforeStart);
		auto leftScore = left.scoreBeforeStart;
		auto rightScore = right.scoreBeforeStart;
		for (int i = 0; i < left.confirmedRows.rows && i < right.confirmedRows.rows; i++)
		{
			Word mask = 1LL << i;
			leftScore += (left.VP & mask) ? 1 : 0;
			leftScore -= (left.VN & mask) ? 1 : 0;
			rightScore += (right.VP & mask) ? 1 : 0;
			rightScore -= (right.VN & mask) ? 1 : 0;
			assert(leftScore == rightScore);
		}
	}
#endif

	void setValue(NodeSlice<WordSlice>& slice, LengthType node, LengthType index, int row, ScoreType value, ScoreType uninitializedValue) const
	{
		if (!slice.hasNode(node))
		{
			slice.addNode(node, params.graph.NodeLength(node));
			auto nodeslice = slice.node(node);
			for (size_t i = 0; i < nodeslice.size(); i++)
			{
				nodeslice[i] = {0, 0, uninitializedValue, uninitializedValue, 0, false};
				nodeslice[i].confirmedRows.partial = false;
			}
		}
		assert(slice.hasNode(node));
		auto nodeslice = slice.node(node);
		assert(nodeslice.size() > index);
		nodeslice[index].setValue(row, value);
	}

	NodeCalculationResult calculateSliceAlternate(const std::string& sequence, size_t startj, NodeSlice<WordSlice>& currentSlice, const DPSlice& previousSlice, std::vector<bool>& processed, int bandwidth) const
	{
		std::vector<std::vector<std::pair<LengthType, LengthType>>> calculables;
		std::vector<std::vector<std::pair<LengthType, LengthType>>> nextCalculables;
		calculables.resize(bandwidth+1);
		nextCalculables.resize(bandwidth+1);

		//usually the calculable lists contain a ton of entries
		//so just preallocate a large block so we don't need to resize too often
		for (size_t i = 0; i < bandwidth+1; i++)
		{
			calculables[i].reserve(params.AlternateMethodCutoff); //semi-arbitrary guess based on the bitvector/alternate cutoff
			nextCalculables[i].reserve(params.AlternateMethodCutoff);
		}

		for (const auto pair : previousSlice.scores)
		{
			auto start = params.graph.NodeStart(pair.first);
			if (startj == 0)
			{
				for (size_t i = 0; i < pair.second.size(); i++)
				{
					if (pair.second[i].scoreEnd < previousSlice.minScore + bandwidth && pair.second[i].scoreEndExists)
					{
						if (characterMatch(sequence[startj], params.graph.NodeSequences(start + i)))
						{
							calculables[pair.second[i].scoreEnd - previousSlice.minScore].emplace_back(pair.first, start+i);
						}
						else
						{
							calculables[pair.second[i].scoreEnd - previousSlice.minScore + 1].emplace_back(pair.first, start+i);
						}
					}
				}
			}
			else
			{
				for (size_t i = 0; i < pair.second.size() - 1; i++)
				{
					if (pair.second[i].scoreEnd < previousSlice.minScore + bandwidth && pair.second[i].scoreEndExists)
					{
						assert(pair.second[i].scoreEnd >= previousSlice.minScore);
						calculables[pair.second[i].scoreEnd - previousSlice.minScore + 1].emplace_back(pair.first, start+i);
						if (characterMatch(sequence[startj], params.graph.NodeSequences(start + i + 1)))
						{
							calculables[pair.second[i].scoreEnd - previousSlice.minScore].emplace_back(pair.first, start+i+1);
						}
						else
						{
							calculables[pair.second[i].scoreEnd - previousSlice.minScore + 1].emplace_back(pair.first, start+i+1);
						}
					}
				}
				if (pair.second.back().scoreEnd < previousSlice.minScore + bandwidth && pair.second.back().scoreEndExists)
				{
					calculables[pair.second.back().scoreEnd - previousSlice.minScore + 1].emplace_back(pair.first, start+pair.second.size()-1);
					for (auto neighbor : params.graph.outNeighbors[pair.first])
					{
						auto u = params.graph.NodeStart(neighbor);
						if (characterMatch(sequence[startj], params.graph.NodeSequences(u)))
						{
							calculables[pair.second.back().scoreEnd - previousSlice.minScore].emplace_back(neighbor, u);
						}
						else
						{
							calculables[pair.second.back().scoreEnd - previousSlice.minScore + 1].emplace_back(neighbor, u);
						}
					}
				}
			}
		}
		assert(calculables[0].size() > 0 || calculables[1].size() > 0);

		std::vector<LengthType> processedlist;
		size_t cellsProcessed = 0;
		ScoreType minScore = previousSlice.minScore;
		for (size_t j = 0; j < WordConfiguration<Word>::WordSize; j++)
		{
			ScoreType scoreIndexPlus = 0;
			if (calculables[0].size() == 0)
			{
				scoreIndexPlus = -1;
			}
			for (int scoreplus = 0; scoreplus < bandwidth; scoreplus++)
			{
				for (auto pair : calculables[scoreplus])
				{
					if (processed[pair.second]) continue;
					cellsProcessed++;
					processed[pair.second] = true;
					processedlist.push_back(pair.second);
					auto nodeStart = params.graph.NodeStart(pair.first);
					auto nodeEnd = params.graph.NodeEnd(pair.first);
					assert(pair.second >= nodeStart);
					assert(pair.second < nodeEnd);
#ifdef EXTRACORRECTNESSASSERTIONS
					WordSlice debugOldSlice;
					bool debugCompare = false;
					if (currentSlice.hasNode(pair.first) && currentSlice.node(pair.first)[pair.second - nodeStart].scoreBeforeStart != sequence.size())
					{
						debugCompare = true;
						debugOldSlice = currentSlice.node(pair.first)[pair.second - nodeStart];
					}
#endif
					setValue(currentSlice, pair.first, pair.second - nodeStart, j, minScore + scoreplus, sequence.size());
#ifdef EXTRACORRECTNESSASSERTIONS
					if (debugCompare)
					{
						WordSlice debugNewSlice = currentSlice.node(pair.first)[pair.second - nodeStart];
						assertBitvectorConfirmedAreConsistent(debugOldSlice, debugNewSlice);
					}
#endif
					assert(currentSlice.node(pair.first)[pair.second - nodeStart].getValue(j) == minScore + scoreplus);
					nextCalculables[scoreplus + 1 + scoreIndexPlus].emplace_back(pair.first, pair.second);
					if (pair.second + 1 == nodeEnd)
					{
						for (auto neighbor : params.graph.outNeighbors[pair.first])
						{
							auto u = params.graph.NodeStart(neighbor);
							if (!processed[u]) calculables[scoreplus+1].emplace_back(neighbor, u);
							if (j < WordConfiguration<Word>::WordSize - 1)
							{
								if (characterMatch(sequence[startj+j+1], params.graph.NodeSequences(u)))
								{
									nextCalculables[scoreplus + scoreIndexPlus].emplace_back(neighbor, u);
								}
								else
								{
									nextCalculables[scoreplus + scoreIndexPlus + 1].emplace_back(neighbor, u);
								}
							}
						}
					}
					else
					{
						auto u = pair.second+1;
						assert(u < nodeEnd);
						if (!processed[u]) calculables[scoreplus+1].emplace_back(pair.first, u);
						if (j < WordConfiguration<Word>::WordSize - 1)
						{
							if (characterMatch(sequence[startj+j+1], params.graph.NodeSequences(u)))
							{
								nextCalculables[scoreplus + scoreIndexPlus].emplace_back(pair.first, u);
							}
							else
							{
								nextCalculables[scoreplus + scoreIndexPlus + 1].emplace_back(pair.first, u);
							}
						}
					}
				}
			}
			if (calculables[0].size() == 0)
			{
				minScore++;
			}
			for (auto cell : processedlist)
			{
				assert(processed[cell]);
				processed[cell] = false;
			}
			processedlist.clear();
			if (j < WordConfiguration<Word>::WordSize - 1)
			{
				std::swap(calculables, nextCalculables);
				for (size_t i = 0; i < nextCalculables.size(); i++)
				{
					nextCalculables[i].clear();
				}
			}
		}
		if (calculables[0].size() == 0) std::swap(calculables[0], calculables[1]);
		assert(calculables[0].size() > 0);
		NodeCalculationResult result;
		result.minScore = minScore;
		for (auto pair : calculables[0])
		{
			result.minScoreIndex.push_back(pair.second);
		}
		result.cellsProcessed = cellsProcessed;
		return result;
	}

	NodeCalculationResult calculateSlice(const std::string& sequence, size_t j, NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<LengthType>& bandOrder, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, std::vector<size_t>& partOfComponent, UniqueQueue<LengthType>& calculables) const
	{
		ScoreType currentMinimumScore = std::numeric_limits<ScoreType>::max();
		std::vector<LengthType> currentMinimumIndex;
		size_t cellsProcessed = 0;

		//preprocessed bitvectors for character equality
		Word BA = WordConfiguration<Word>::AllZeros;
		Word BT = WordConfiguration<Word>::AllZeros;
		Word BC = WordConfiguration<Word>::AllZeros;
		Word BG = WordConfiguration<Word>::AllZeros;
		for (int i = 0; i < WordConfiguration<Word>::WordSize && j+i < sequence.size(); i++)
		{
			Word mask = ((Word)1) << i;
			if (characterMatch(sequence[j+i], 'A')) BA |= mask;
			if (characterMatch(sequence[j+i], 'C')) BC |= mask;
			if (characterMatch(sequence[j+i], 'T')) BT |= mask;
			if (characterMatch(sequence[j+i], 'G')) BG |= mask;
		}
		assert((BA | BC | BT | BG) == WordConfiguration<Word>::AllOnes);
		EqVector EqV {BA, BT, BC, BG};
		auto components = getStronglyConnectedComponents(bandOrder, currentBand);
		for (size_t i = 0; i < components.size(); i++)
		{
			for (auto node : components[i])
			{
				partOfComponent[node] = i;
			}
		}
		for (size_t component = components.size()-1; component < components.size(); component--)
		{
			forceComponentZeroRow(currentSlice, previousSlice, currentBand, previousBand, components[component], component, partOfComponent, sequence.size());
			assert(calculables.size() == 0);
			calculables.insert(components[component].begin(), components[component].end());
			while (calculables.size() > 0)
			{
				auto i = calculables.top();
				assert(currentBand[i]);
				calculables.pop();
				auto oldEnd = currentSlice.node(i).back();
#ifdef EXTRACORRECTNESSASSERTIONS
				auto debugOldNode = currentSlice.node(i);
#endif
				auto nodeCalc = calculateNode(i, j, sequence, EqV, currentSlice, previousSlice, currentBand, previousBand);
				currentSlice.setMinScore(i, nodeCalc.minScore);
				auto newEnd = currentSlice.node(i).back();
#ifdef EXTRACORRECTNESSASSERTIONS
				auto debugNewNode = currentSlice.node(i);
				for (size_t debugi = 0; debugi < debugOldNode.size(); debugi++)
				{
					assertBitvectorConfirmedAreConsistent(debugNewNode[debugi], debugOldNode[debugi]);
					assert(debugNewNode[debugi].confirmedRows >= debugOldNode[debugi].confirmedRows);
				}
#endif
				assert(newEnd.scoreBeforeStart == oldEnd.scoreBeforeStart);
				assert(newEnd.confirmedRows >= oldEnd.confirmedRows);
				if (newEnd.scoreBeforeStart < sequence.size() && newEnd.confirmedRows > oldEnd.confirmedRows)
				{
					for (auto neighbor : params.graph.outNeighbors[i])
					{
						if (partOfComponent[neighbor] != component) continue;
						if (currentSlice.node(neighbor)[0].confirmedRows.rows < WordConfiguration<Word>::WordSize)
						{
							calculables.insert(neighbor);
						}
					}
				}
#ifndef NDEBUG
				auto debugslice = currentSlice.node(i);
				if (nodeCalc.minScore != std::numeric_limits<ScoreType>::max())
				{
					for (auto index : nodeCalc.minScoreIndex)
					{
						assert(index >= params.graph.NodeStart(i));
						assert(index < params.graph.NodeEnd(i));
						assert(debugslice[index - params.graph.NodeStart(i)].scoreEnd == nodeCalc.minScore);
					}
				}
#endif
				if (nodeCalc.minScore < currentMinimumScore)
				{
					currentMinimumScore = nodeCalc.minScore;
					currentMinimumIndex.clear();
				}
				if (nodeCalc.minScore == currentMinimumScore)
				{
					currentMinimumIndex.insert(currentMinimumIndex.end(), nodeCalc.minScoreIndex.begin(), nodeCalc.minScoreIndex.end());
				}
				cellsProcessed += nodeCalc.cellsProcessed;
			}
#ifndef NDEBUG
			for (auto node : components[component])
			{
				assert(currentSlice.node(node)[0].confirmedRows.rows == WordConfiguration<Word>::WordSize);
			}
#endif
		}
		for (size_t i = 0; i < components.size(); i++)
		{
			for (auto node : components[i])
			{
				partOfComponent[node] = std::numeric_limits<size_t>::max();
			}
		}

#ifdef EXTRACORRECTNESSASSERTIONS
		for (auto pair : currentSlice)
		{
			for (auto& word : pair.second)
			{
				word.confirmedRows.exists = -1;
			}
		}
#endif

		NodeCalculationResult result;
		result.minScore = currentMinimumScore;
		result.minScoreIndex = currentMinimumIndex;
		result.cellsProcessed = cellsProcessed;
		return result;
	}

	DPSlice extendDPSlice(const DPSlice& previous, const std::vector<bool>& previousBand, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap, int bandwidth) const
	{
		DPSlice result { &nodesliceMap };
		result.j = previous.j + WordConfiguration<Word>::WordSize;
		result.correctness = previous.correctness;
		result.nodes = rowBandFunction(previous, previousBand, bandwidth);
		assert(result.nodes.size() > 0);
		return result;
	}

	void fillDPSlice(const std::string& sequence, DPSlice& slice, const DPSlice& previousSlice, const std::vector<bool>& previousBand, std::vector<size_t>& partOfComponent, const std::vector<bool>& currentBand, UniqueQueue<LengthType>& calculables) const
	{
		auto sliceResult = calculateSlice(sequence, slice.j, slice.scores, previousSlice.scores, slice.nodes, currentBand, previousBand, partOfComponent, calculables);
		slice.cellsProcessed = sliceResult.cellsProcessed;
		slice.minScoreIndex = sliceResult.minScoreIndex;
		slice.minScore = sliceResult.minScore;
		assert(slice.minScore >= previousSlice.minScore);
		slice.correctness = slice.correctness.NextState(slice.minScore - previousSlice.minScore, WordConfiguration<Word>::WordSize);
	}

	DPSlice pickMethodAndExtendFill(const std::string& sequence, const DPSlice& previous, const std::vector<bool>& previousBand, std::vector<bool>& currentBand, std::vector<size_t>& partOfComponent, UniqueQueue<LengthType>& calculables, std::vector<bool>& processed, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap, int bandwidth) const
	{
		{ //braces so bandTest doesn't take memory later
			auto bandTest = extendDPSlice(previous, previousBand, nodesliceMap, bandwidth);
			assert(sequence.size() >= bandTest.j + WordConfiguration<Word>::WordSize);
			size_t cells = 0;
			for (auto node : bandTest.nodes)
			{
				cells += params.graph.NodeLength(node);
			}
			if (cells < params.AlternateMethodCutoff)
			{
				bandTest.scores.reserve(cells);
				for (auto node : bandTest.nodes)
				{
					bandTest.scores.addNode(node, params.graph.NodeLength(node));
					currentBand[node] = true;
				}
				fillDPSlice(sequence, bandTest, previous, previousBand, partOfComponent, currentBand, calculables);
				bandTest.numCells = cells;

#ifdef EXTRACORRECTNESSASSERTIONS
				verifySliceBitvector(sequence, bandTest, previous);
#endif
				return bandTest;
			}
		}
		{
			DPSlice result { &nodesliceMap };
			result.j = previous.j + WordConfiguration<Word>::WordSize;
			result.correctness = previous.correctness;
			result.scores.reserve(params.AlternateMethodCutoff);

			auto sliceResult = calculateSliceAlternate(sequence, result.j, result.scores, previous, processed, bandwidth);
			result.cellsProcessed = sliceResult.cellsProcessed;
			result.minScoreIndex = sliceResult.minScoreIndex;
			result.minScore = sliceResult.minScore;
			assert(result.minScore >= previous.minScore);
			result.correctness = result.correctness.NextState(result.minScore - previous.minScore, WordConfiguration<Word>::WordSize);

#ifdef EXTRACORRECTNESSASSERTIONS
			verifySliceAlternate(sequence, result, previous, false, bandwidth);
#endif

			finalizeAlternateSlice(result, currentBand, sequence.size(), bandwidth);

			return result;
		}
	}

	void finalizeAlternateSlice(DPSlice& slice, std::vector<bool>& currentBand, ScoreType uninitializedValue, int bandwidth) const
	{
		for (auto pair : slice.scores)
		{
			auto node = pair.first;
			slice.nodes.push_back(node);
			assert(!currentBand[node]);
			currentBand[node] = true;
			ScoreType minScore = pair.second[0].scoreEnd;
			for (auto& word : pair.second)
			{
				assert(word.confirmedRows.rows <= WordConfiguration<Word>::WordSize - 1);
				assert(word.confirmedRows.rows >= 0);
				word.scoreEndExists = word.confirmedRows.rows == WordConfiguration<Word>::WordSize - 1;
				word.confirmedRows.rows = WordConfiguration<Word>::WordSize;
				word.confirmedRows.partial = false;
				minScore = std::min(minScore, word.scoreEnd);
			}
			for (auto& word : pair.second)
			{
				if (word.scoreEnd == uninitializedValue)
				{
					word.scoreEnd = minScore + pair.second.size() + bandwidth + 1;
					word.scoreBeforeStart = minScore + pair.second.size() + bandwidth + 1;
				}
			}
			slice.numCells += pair.second.size();
			slice.scores.setMinScore(node, minScore);
		}
	}

	void removeWronglyAlignedEnd(DPTable& table) const
	{
		bool currentlyCorrect = table.correctness.back().CurrentlyCorrect();
		while (!currentlyCorrect)
		{
			table.correctness.pop_back();
			table.bandwidthPerSlice.pop_back();
			if (table.correctness.size() == 0) break;
			currentlyCorrect = table.correctness.back().FalseFromCorrect();
		}
		if (table.correctness.size() == 0)
		{
			table.slices.clear();
		}
		while (table.slices.size() > 1 && table.slices.back().j >= table.correctness.size() * WordConfiguration<Word>::WordSize) table.slices.pop_back();
	}

	DPTable getSqrtSlices(const std::string& sequence, const DPSlice& initialSlice, size_t numSlices, size_t samplingFrequency, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		assert(initialSlice.j == -WordConfiguration<Word>::WordSize);
		assert(initialSlice.j + numSlices * WordConfiguration<Word>::WordSize <= sequence.size());
		DPTable result;
		size_t realCells = 0;
		size_t cellsProcessed = 0;
		result.samplingFrequency = samplingFrequency;
		std::vector<bool> previousBand;
		std::vector<bool> currentBand;
		std::vector<size_t> partOfComponent;
		UniqueQueue<LengthType> calculables { params.graph.NodeSize() };
		previousBand.resize(params.graph.NodeSize(), false);
		currentBand.resize(params.graph.NodeSize(), false);
		partOfComponent.resize(params.graph.NodeSize(), std::numeric_limits<size_t>::max());
		{
			auto initialOrder = initialSlice.nodes;
			for (auto node : initialOrder)
			{
				previousBand[node] = true;
			}
		}
#ifndef NDEBUG
		debugLastRowMinScore = 0;
#endif
		DPSlice lastSlice = initialSlice.getFrozenSqrtEndScores();
		DPSlice storeSlice = lastSlice;
		assert(lastSlice.correctness.CurrentlyCorrect());
		DPSlice rampSlice = lastSlice;
		std::vector<bool> processed;
		processed.resize(params.graph.SizeInBp(), false);
		size_t rampRedoIndex = -1;
		size_t rampUntil = 0;
		DPSlice backtraceOverridePreslice = lastSlice;
		std::vector<DPSlice> backtraceOverrideTemps;
		bool backtraceOverriding = false;
#ifndef NDEBUG
		volatile size_t debugLastProcessedSlice;
#endif
		for (size_t slice = 0; slice < numSlices; slice++)
		{
			int bandwidth = (rampUntil >= slice) ? params.rampBandwidth : params.initialBandwidth;
			size_t storeSliceIndex = slice / samplingFrequency + 1;
#ifndef NDEBUG
			debugLastProcessedSlice = slice;
			debugLastRowMinScore = lastSlice.minScore;
#endif
			auto timeStart = std::chrono::system_clock::now();
			auto newSlice = pickMethodAndExtendFill(sequence, lastSlice, previousBand, currentBand, partOfComponent, calculables, processed, nodesliceMap, bandwidth);
			auto timeEnd = std::chrono::system_clock::now();
			auto time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
#ifdef SLICEVERBOSE
			std::cerr << "slice " << slice << " bandwidth " << bandwidth << " time " << time << " cells " << newSlice.numCells;
#endif

			if (rampUntil == slice && newSlice.numCells >= params.BacktraceOverrideCutoff)
			{
				rampUntil++;
			}
			if ((rampUntil == slice-1 || (rampUntil < slice && newSlice.correctness.CurrentlyCorrect() && newSlice.correctness.FalseFromCorrect())) && lastSlice.numCells < params.BacktraceOverrideCutoff)
			{
				rampSlice = lastSlice;
				rampRedoIndex = slice-1;
			}
			assert(newSlice.j == lastSlice.j + WordConfiguration<Word>::WordSize);

			realCells += newSlice.numCells;
			cellsProcessed += newSlice.cellsProcessed;

			if (!newSlice.correctness.CorrectFromCorrect())
			{
				newSlice.scores.clearVectorMap();
#ifndef NDEBUG
				debugLastProcessedSlice = slice-1;
#endif
				break;
			}
			if (!newSlice.correctness.CurrentlyCorrect() && rampUntil < slice && params.rampBandwidth > params.initialBandwidth)
			{
				for (auto node : newSlice.nodes)
				{
					assert(currentBand[node]);
					currentBand[node] = false;
				}
				for (auto node : lastSlice.nodes)
				{
					assert(previousBand[node]);
					previousBand[node] = false;
				}
				newSlice.scores.clearVectorMap();
				rampUntil = slice;
				std::swap(slice, rampRedoIndex);
				std::swap(lastSlice, rampSlice);
				for (auto node : lastSlice.nodes)
				{
					assert(!previousBand[node]);
					previousBand[node] = true;
				}
				while (result.bandwidthPerSlice.size() > slice+1) result.bandwidthPerSlice.pop_back();
				while (result.correctness.size() > slice+1) result.correctness.pop_back();
				while (result.slices.size() > 1 && result.slices.back().j > slice * WordConfiguration<Word>::WordSize) result.slices.pop_back();
#ifdef SLICEVERBOSE
				std::cerr << " ramp to " << slice;
#endif
				if (backtraceOverriding)
				{
#ifdef SLICEVERBOSE
					std::cerr << " preslicej " << backtraceOverridePreslice.j << " lastslicej " << lastSlice.j;
#endif
					if (backtraceOverridePreslice.j > lastSlice.j)
					{
#ifdef SLICEVERBOSE
						std::cerr << " empty backtrace override";
#endif
						backtraceOverriding = false;
						//empty memory
						{
							decltype(backtraceOverrideTemps) tmp;
							std::swap(backtraceOverrideTemps, tmp);
						}
					}
					else
					{
#ifdef SLICEVERBOSE
						std::cerr << " shorten backtrace override";
#endif
						while (backtraceOverrideTemps.size() > 0 && backtraceOverrideTemps.back().j > lastSlice.j)
						{
							DPSlice tmp;
							std::swap(backtraceOverrideTemps.back(), tmp);
						}
#ifdef SLICEVERBOSE
						std::cerr << " to " << backtraceOverrideTemps.size() << " temps";
#endif
					}
				}
				while (result.backtraceOverrides.size() > 0 && result.backtraceOverrides.back().endj > lastSlice.j)
				{
					result.backtraceOverrides.pop_back();
				}
#ifdef SLICEVERBOSE
				std::cerr << std::endl;
				std::cerr << "bandwidthPerSlice.size() " << result.bandwidthPerSlice.size();
				if (result.slices.size() > 0) std::cerr << " slices.back().j " << result.slices.back().j; else std::cerr << " slices.size() 0";
				if (result.backtraceOverrides.size() > 0) std::cerr << " backtraceOverrides.back().endj " << result.backtraceOverrides.back().endj; else std::cerr << " backtraceOverrides.size() 0";
				std::cerr << std::endl;
#endif
				continue;
			}

			if (!backtraceOverriding && newSlice.numCells >= params.BacktraceOverrideCutoff && lastSlice.numCells < params.BacktraceOverrideCutoff)
			{
#ifdef SLICEVERBOSE
				std::cerr << " start backtrace override";
#endif
				assert(lastSlice.numCells < params.BacktraceOverrideCutoff);
				backtraceOverridePreslice = lastSlice;
				backtraceOverriding = true;
				backtraceOverrideTemps.push_back(newSlice.getFrozenScores());
			}
			else if (backtraceOverriding)
			{
				if (newSlice.numCells < params.BacktraceOverrideCutoff)
				{
#ifdef SLICEVERBOSE
					std::cerr << " end backtrace override";
#endif
					assert(lastSlice.j == backtraceOverrideTemps.back().j);
					assert(backtraceOverrideTemps.size() > 0);
					result.backtraceOverrides.emplace_back(params, sequence, backtraceOverridePreslice, backtraceOverrideTemps);
					backtraceOverriding = false;
					while (result.slices.size() > 0 && result.slices.back().j >= result.backtraceOverrides.back().startj && result.slices.back().j <= result.backtraceOverrides.back().endj)
					{
						result.slices.pop_back();
					}
					result.slices.push_back(lastSlice);
#ifdef SLICEVERBOSE
					std::cerr << " push slice j " << lastSlice.j;
#endif
					storeSlice = newSlice.getFrozenSqrtEndScores();
					//empty memory
					{
						decltype(backtraceOverrideTemps) tmp;
						std::swap(backtraceOverrideTemps, tmp);
					}
				}
				else
				{
#ifdef SLICEVERBOSE
					std::cerr << " continue backtrace override";
#endif
					backtraceOverrideTemps.push_back(newSlice.getFrozenScores());
				}
			}
#ifdef SLICEVERBOSE
			std::cerr << std::endl;
#endif

			assert(result.bandwidthPerSlice.size() == slice);
			result.bandwidthPerSlice.push_back(bandwidth);
			result.correctness.push_back(newSlice.correctness);
			if (slice % samplingFrequency == 0)
			{
				if (result.slices.size() == 0 || storeSlice.j != result.slices.back().j)
				{
					result.slices.push_back(storeSlice);
#ifdef SLICEVERBOSE
					std::cerr << " push slice j " << storeSlice.j;
#endif
					storeSlice = newSlice.getFrozenSqrtEndScores();
				}
			}
			if (newSlice.EstimatedMemoryUsage() < storeSlice.EstimatedMemoryUsage())
			{
				storeSlice = newSlice.getFrozenSqrtEndScores();
			}
			for (auto node : lastSlice.nodes)
			{
				assert(previousBand[node]);
				previousBand[node] = false;
			}
			assert(newSlice.minScore != std::numeric_limits<LengthType>::max());
			assert(newSlice.minScore >= lastSlice.minScore);
#ifndef NDEBUG
			for (auto index : newSlice.minScoreIndex)
			{
				auto debugMinimumNode = params.graph.IndexToNode(index);
				assert(newSlice.scores.hasNode(debugMinimumNode));
				auto debugslice = newSlice.scores.node(debugMinimumNode);
				assert(index >= params.graph.NodeStart(debugMinimumNode));
				assert(index < params.graph.NodeEnd(debugMinimumNode));
				assert(debugslice[index - params.graph.NodeStart(debugMinimumNode)].scoreEnd == newSlice.minScore);
			}
#endif
			lastSlice = newSlice.getFrozenSqrtEndScores();
			newSlice.scores.clearVectorMap();
			std::swap(previousBand, currentBand);
		}

		if (backtraceOverriding)
		{
			assert(backtraceOverrideTemps.size() > 0);
			assert(lastSlice.j == backtraceOverrideTemps.back().j);
			result.backtraceOverrides.emplace_back(params, sequence, backtraceOverridePreslice, backtraceOverrideTemps);
			backtraceOverriding = false;
			//empty memory
			{
				decltype(backtraceOverrideTemps) tmp;
				std::swap(backtraceOverrideTemps, tmp);
			}
			while (result.slices.size() > 0 && result.slices.back().j >= result.backtraceOverrides.back().startj && result.slices.back().j <= result.backtraceOverrides.back().endj)
			{
				result.slices.pop_back();
			}
		}
#ifndef NDEBUF
		volatile
#endif
		size_t lastExisting = 0;
		// assert(debugLastProcessedSlice == -1 || lastExisting == debugLastProcessedSlice / samplingFrequency || lastExisting == debugLastProcessedSlice / samplingFrequency + 1);
		// storeSlices.erase(storeSlices.begin() + lastExisting + 1, storeSlices.end());
		// result.slices = storeSlices;
		assert(result.bandwidthPerSlice.size() == debugLastProcessedSlice + 1);
#ifndef NDEBUG
		assert(result.slices.size() > 0);
		for (size_t i = 0; i < result.slices.size(); i++)
		{
			// assert(i == 0 || result.slices[i].j / WordConfiguration<Word>::WordSize / samplingFrequency == i-1);
			assert(i <= 1 || result.slices[i].j > result.slices[i-1].j);
			// assert(i != 1 || result.slices[i].j >= 0);
		}
		for (size_t i = 1; i < result.slices.size(); i++)
		{
			assert(result.slices[i].minScore >= result.slices[i-1].minScore);
		}
		for (size_t i = 0; i < result.backtraceOverrides.size(); i++)
		{
			assert(result.backtraceOverrides[i].endj >= result.backtraceOverrides[i].startj);
		}
		for (size_t i = 1; i < result.backtraceOverrides.size(); i++)
		{
			assert(result.backtraceOverrides[i].startj > result.backtraceOverrides[i-1].endj);
		}
#endif
		return result;
	}

	std::vector<DPSlice> getSlicesFromTable(const std::string& sequence, LengthType overrideLastJ, const DPTable& table, size_t startIndex, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		assert(startIndex < table.slices.size());
		size_t startSlice = (table.slices[startIndex].j + WordConfiguration<Word>::WordSize) / WordConfiguration<Word>::WordSize;
		assert(overrideLastJ > startSlice * WordConfiguration<Word>::WordSize);
		size_t endSlice;
		if (startIndex == table.slices.size()-1) endSlice = table.bandwidthPerSlice.size(); else endSlice = (table.slices[startIndex+1].j + WordConfiguration<Word>::WordSize) / WordConfiguration<Word>::WordSize;
		if (endSlice * WordConfiguration<Word>::WordSize >= overrideLastJ) endSlice = (overrideLastJ / WordConfiguration<Word>::WordSize);
		assert(endSlice > startSlice);
		assert(endSlice <= table.bandwidthPerSlice.size());
		assert(startIndex < table.slices.size());
		const auto& initialSlice = table.slices[startIndex];
		std::vector<DPSlice> result;
		size_t realCells = 0;
		size_t cellsProcessed = 0;
		std::vector<bool> previousBand;
		std::vector<bool> currentBand;
		std::vector<size_t> partOfComponent;
		UniqueQueue<LengthType> calculables { params.graph.NodeSize() };
		previousBand.resize(params.graph.NodeSize(), false);
		currentBand.resize(params.graph.NodeSize(), false);
		partOfComponent.resize(params.graph.NodeSize(), std::numeric_limits<size_t>::max());
		{
			auto initialOrder = initialSlice.nodes;
			for (auto node : initialOrder)
			{
				previousBand[node] = true;
			}
		}
#ifndef NDEBUG
		debugLastRowMinScore = 0;
#endif
		DPSlice lastSlice = initialSlice.getFrozenSqrtEndScores();
		// assert(lastSlice.correctness.CurrentlyCorrect());
		DPSlice rampSlice = lastSlice;
		std::vector<bool> processed;
		processed.resize(params.graph.SizeInBp(), false);
		for (size_t slice = startSlice; slice < endSlice; slice++)
		{
			int bandwidth = table.bandwidthPerSlice[slice];
#ifndef NDEBUG
			debugLastRowMinScore = lastSlice.minScore;
#endif
			auto newSlice = pickMethodAndExtendFill(sequence, lastSlice, previousBand, currentBand, partOfComponent, calculables, processed, nodesliceMap, bandwidth);
			assert(result.size() == 0 || newSlice.j == result.back().j + WordConfiguration<Word>::WordSize);

			size_t sliceCells = 0;
			for (auto node : newSlice.nodes)
			{
				sliceCells += params.graph.NodeEnd(node) - params.graph.NodeStart(node);
			}
			realCells += sliceCells;
			cellsProcessed += newSlice.cellsProcessed;

			// assert(slice == endSlice-1 || newSlice.correctness.CurrentlyCorrect());
			result.push_back(newSlice.getFrozenScores());
			for (auto node : lastSlice.nodes)
			{
				assert(previousBand[node]);
				previousBand[node] = false;
			}
			assert(newSlice.minScore != std::numeric_limits<LengthType>::max());
			assert(newSlice.minScore >= lastSlice.minScore);
#ifndef NDEBUG
			for (auto index : newSlice.minScoreIndex)
			{
				auto debugMinimumNode = params.graph.IndexToNode(index);
				assert(newSlice.scores.hasNode(debugMinimumNode));
				auto debugslice = newSlice.scores.node(debugMinimumNode);
				assert(index >= params.graph.NodeStart(debugMinimumNode));
				assert(index < params.graph.NodeEnd(debugMinimumNode));
				assert(debugslice[index - params.graph.NodeStart(debugMinimumNode)].scoreEnd == newSlice.minScore);
			}
#endif
			lastSlice = newSlice.getFrozenSqrtEndScores();
			newSlice.scores.clearVectorMap();
			std::swap(previousBand, currentBand);
		}
#ifndef NDEBUG
		for (size_t i = 1; i < result.size(); i++)
		{
			assert(result[i].minScore >= result[i-1].minScore);
		}
#endif
		return result;
	}

	DPSlice getInitialSliceOnlyOneNode(LengthType nodeIndex) const
	{
		DPSlice result;
		result.j = -WordConfiguration<Word>::WordSize;
		result.scores.addNode(nodeIndex, params.graph.NodeEnd(nodeIndex) - params.graph.NodeStart(nodeIndex));
		result.scores.setMinScore(nodeIndex, 0);
		result.minScore = 0;
		result.minScoreIndex.push_back(params.graph.NodeEnd(nodeIndex) - 1);
		result.nodes.push_back(nodeIndex);
		auto slice = result.scores.node(nodeIndex);
		for (size_t i = 0; i < slice.size(); i++)
		{
			slice[i] = {0, 0, 0, 0, WordConfiguration<Word>::WordSize, false};
		}
		return result;
	}

	int getSamplingFrequency(size_t sequenceLen) const
	{
		size_t samplingFrequency = 1;
		samplingFrequency = (int)(sqrt(sequenceLen / WordConfiguration<Word>::WordSize));
		return samplingFrequency;
	}

	TwoDirectionalSplitAlignment getSplitAlignment(const std::string& sequence, LengthType matchBigraphNodeId, bool matchBigraphNodeBackwards, LengthType matchSequencePosition, ScoreType maxScore, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		assert(matchSequencePosition >= 0);
		assert(matchSequencePosition < sequence.size());
		size_t forwardNode;
		size_t backwardNode;
		TwoDirectionalSplitAlignment result;
		result.sequenceSplitIndex = matchSequencePosition;
		if (matchBigraphNodeBackwards)
		{
			forwardNode = params.graph.nodeLookup.at(matchBigraphNodeId * 2 + 1);
			backwardNode = params.graph.nodeLookup.at(matchBigraphNodeId * 2);
		}
		else
		{
			forwardNode = params.graph.nodeLookup.at(matchBigraphNodeId * 2);
			backwardNode = params.graph.nodeLookup.at(matchBigraphNodeId * 2 + 1);
		}
		assert(params.graph.NodeEnd(forwardNode) - params.graph.NodeStart(forwardNode) == params.graph.NodeEnd(backwardNode) - params.graph.NodeStart(backwardNode));
		ScoreType score = 0;
		if (matchSequencePosition > 0)
		{
			assert(sequence.size() >= matchSequencePosition + params.graph.DBGOverlap);
			auto backwardPart = CommonUtils::ReverseComplement(sequence.substr(0, matchSequencePosition + params.graph.DBGOverlap));
			int backwardpadding = (WordConfiguration<Word>::WordSize - (backwardPart.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
			assert(backwardpadding < WordConfiguration<Word>::WordSize);
			for (int i = 0; i < backwardpadding; i++)
			{
				backwardPart += 'N';
			}
			auto backwardInitialBand = getInitialSliceOnlyOneNode(backwardNode);
			size_t samplingFrequency = getSamplingFrequency(backwardPart.size());
			auto backwardSlice = getSqrtSlices(backwardPart, backwardInitialBand, backwardPart.size() / WordConfiguration<Word>::WordSize, samplingFrequency, nodesliceMap);
			removeWronglyAlignedEnd(backwardSlice);
			result.backward = std::move(backwardSlice);
			if (result.backward.slices.size() > 0) score += result.backward.slices.back().minScore;
		}
		if (matchSequencePosition < sequence.size() - 1)
		{
			auto forwardPart = sequence.substr(matchSequencePosition);
			int forwardpadding = (WordConfiguration<Word>::WordSize - (forwardPart.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
			assert(forwardpadding < WordConfiguration<Word>::WordSize);
			for (int i = 0; i < forwardpadding; i++)
			{
				forwardPart += 'N';
			}
			auto forwardInitialBand = getInitialSliceOnlyOneNode(forwardNode);
			size_t samplingFrequency = getSamplingFrequency(forwardPart.size());
			auto forwardSlice = getSqrtSlices(forwardPart, forwardInitialBand, forwardPart.size() / WordConfiguration<Word>::WordSize, samplingFrequency, nodesliceMap);
			removeWronglyAlignedEnd(forwardSlice);
			result.forward = std::move(forwardSlice);
			if (result.forward.slices.size() > 0) score += result.forward.slices.back().minScore;
		}
		assert(score <= sequence.size() + WordConfiguration<Word>::WordSize * 2);
		return result;
	}

	std::vector<MatrixPosition> reverseTrace(std::vector<MatrixPosition> trace, LengthType end) const
	{
		if (trace.size() == 0) return trace;
		std::reverse(trace.begin(), trace.end());
		for (size_t i = 0; i < trace.size(); i++)
		{
			trace[i].first = params.graph.GetReversePosition(trace[i].first);
			assert(trace[i].second <= end);
			trace[i].second = end - trace[i].second;
		}
		return trace;
	}

	std::pair<std::tuple<ScoreType, std::vector<MatrixPosition>>, std::tuple<ScoreType, std::vector<MatrixPosition>>> getPiecewiseTracesFromSplit(const TwoDirectionalSplitAlignment& split, const std::string& sequence, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		assert(split.sequenceSplitIndex >= 0);
		assert(split.sequenceSplitIndex < sequence.size());
		std::pair<ScoreType, std::vector<MatrixPosition>> backtraceresult {0, std::vector<MatrixPosition>{}};
		std::pair<ScoreType, std::vector<MatrixPosition>> reverseBacktraceResult {0, std::vector<MatrixPosition>{}};
		if (split.sequenceSplitIndex < sequence.size() - 1 && split.forward.slices.size() > 0)
		{
			std::string backtraceSequence;
			auto endpartsize = sequence.size() - split.sequenceSplitIndex;
			int endpadding = (WordConfiguration<Word>::WordSize - (endpartsize % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
			assert(sequence.size() >= split.sequenceSplitIndex + params.graph.DBGOverlap);
			size_t backtraceableSize = sequence.size() - split.sequenceSplitIndex - params.graph.DBGOverlap;
			backtraceSequence = sequence.substr(split.sequenceSplitIndex);
			backtraceSequence.reserve(sequence.size() + endpadding);
			for (int i = 0; i < endpadding; i++)
			{
				backtraceSequence += 'N';
			}
			assert(backtraceSequence.size() % WordConfiguration<Word>::WordSize == 0);

			backtraceresult = getTraceFromTable(backtraceSequence, split.forward, nodesliceMap);
			// std::cerr << "fw score: " << std::get<0>(backtraceresult) << std::endl;

			while (backtraceresult.second.size() > 0 && backtraceresult.second.back().second >= backtraceableSize)
			{
				backtraceresult.second.pop_back();
			}
		}
		if (split.sequenceSplitIndex > 0 && split.backward.slices.size() > 0)
		{
			std::string backwardBacktraceSequence;
			auto startpartsize = split.sequenceSplitIndex;
			assert(sequence.size() >= split.sequenceSplitIndex + params.graph.DBGOverlap);
			size_t backtraceableSize = split.sequenceSplitIndex;
			backwardBacktraceSequence = CommonUtils::ReverseComplement(sequence.substr(0, split.sequenceSplitIndex + params.graph.DBGOverlap));
			int startpadding = (WordConfiguration<Word>::WordSize - (backwardBacktraceSequence.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
			backwardBacktraceSequence.reserve(sequence.size() + startpadding);
			for (int i = 0; i < startpadding; i++)
			{
				backwardBacktraceSequence += 'N';
			}
			assert(backwardBacktraceSequence.size() % WordConfiguration<Word>::WordSize == 0);

			reverseBacktraceResult = getTraceFromTable(backwardBacktraceSequence, split.backward, nodesliceMap);
			// std::cerr << "bw score: " << std::get<0>(reverseBacktraceResult) << std::endl;

			while (reverseBacktraceResult.second.size() > 0 && reverseBacktraceResult.second.back().second >= backtraceableSize)
			{
				reverseBacktraceResult.second.pop_back();
			}
			reverseBacktraceResult.second = reverseTrace(reverseBacktraceResult.second, split.sequenceSplitIndex - 1);
			for (size_t i = 0; i < backtraceresult.second.size(); i++)
			{
				backtraceresult.second[i].second += split.sequenceSplitIndex;
			}
		}

		return std::make_pair(backtraceresult, reverseBacktraceResult);
	}

	std::tuple<ScoreType, std::vector<MatrixPosition>, size_t> getBacktraceFullStart(std::string sequence, std::vector<typename NodeSlice<WordSlice>::MapItem>& nodesliceMap) const
	{
		int padding = (WordConfiguration<Word>::WordSize - (sequence.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		for (int i = 0; i < padding; i++)
		{
			sequence += 'N';
		}
		DPSlice startSlice;
		for (size_t i = 0; i < params.graph.nodeStart.size(); i++)
		{
			startSlice.scores.addNode(i, params.graph.NodeEnd(i) - params.graph.NodeStart(i));
			startSlice.scores.setMinScore(i, 0);
			startSlice.j = -WordConfiguration<Word>::WordSize;
			startSlice.nodes.push_back(i);
			auto slice = startSlice.scores.node(i);
			for (size_t ii = 0; ii < slice.size(); ii++)
			{
				slice[ii] = {0, 0, 0, 0, WordConfiguration<Word>::WordSize, false};
			}
		}
		size_t samplingFrequency = getSamplingFrequency(sequence.size());
		auto slice = getSqrtSlices(sequence, startSlice, sequence.size() / WordConfiguration<Word>::WordSize, samplingFrequency, nodesliceMap);
		removeWronglyAlignedEnd(slice);
		// std::cerr << "score: " << slice.slices.back().minScore << std::endl;

		auto backtraceresult = getTraceFromTable(sequence, slice, nodesliceMap);
		while (backtraceresult.second.back().second >= sequence.size() - padding)
		{
			backtraceresult.second.pop_back();
		}
		assert(backtraceresult.second[0].second == 0);
		assert(backtraceresult.second.back().second == sequence.size() - padding - 1);
		return std::make_tuple(backtraceresult.first, backtraceresult.second, 0);
	}

};

#endif