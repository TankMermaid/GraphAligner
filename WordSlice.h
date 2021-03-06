#ifndef WordSlice_h
#define WordSlice_h

template <typename Word>
class WordConfiguration
{
};

template <>
class WordConfiguration<uint64_t>
{
public:
	static constexpr int WordSize = 64;
	//number of bits per chunk
	//prefix sum differences are calculated in chunks of log w bits
	static constexpr int ChunkBits = 8;
	static constexpr uint64_t AllZeros = 0x0000000000000000;
	static constexpr uint64_t AllOnes = 0xFFFFFFFFFFFFFFFF;
	//positions of the sign bits for each chunk
	static constexpr uint64_t SignMask = 0x8080808080808080;
	//constant for multiplying the chunk popcounts into prefix sums
	//this should be 1 at the start of each chunk
	static constexpr uint64_t PrefixSumMultiplierConstant = 0x0101010101010101;
	//positions of the least significant bits for each chunk
	static constexpr uint64_t LSBMask = 0x0101010101010101;

	static int popcount(uint64_t x)
	{
		//https://en.wikipedia.org/wiki/Hamming_weight
		x -= (x >> 1) & 0x5555555555555555;
		x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
		x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;
		return (x * 0x0101010101010101) >> 56;
	}

	static uint64_t ChunkPopcounts(uint64_t value)
	{
		uint64_t x = value;
		x -= (x >> 1) & 0x5555555555555555;
		x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
		x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;
		return x;
	}

	static int BitPosition(uint64_t low, uint64_t high, int rank)
	{
		assert(rank >= 0);
		auto result = BitPosition(low, rank);
		if (result < 64) return result;
		return 64 + BitPosition(high, result - 64);
	}

	static int BitPosition(uint64_t number, int rank)
	{
		uint64_t bytes = ChunkPopcounts(number);
		//cumulative popcount of each byte
		uint64_t cumulative = bytes * PrefixSumMultiplierConstant;
		//rank is higher than the total number of ones
		if (rank >= (cumulative >> 56))
		{
			rank -= cumulative >> 56;
			return 64 + rank;
		}
		//spread the rank into each byte
		uint64_t rankFinder = ((rank + 1) & 0xFF) * PrefixSumMultiplierConstant;
		//rankMask's msb will be 0 if the c. popcount at that byte is < rank, or 1 if >= rank
		uint64_t rankMask = (cumulative | SignMask) - rankFinder;
		//the total number of ones in rankMask is the number of bytes whose c. popcount is >= rank
		//8 - that is the number of bytes whose c. popcount is < rank
		int smallerBytes = 8 - ((((rankMask & SignMask) >> 7) * PrefixSumMultiplierConstant) >> 56);
		assert(smallerBytes < 8);
		//the bit position will be inside this byte
		uint64_t interestingByte = (number >> (smallerBytes * 8)) & 0xFF;
		if (smallerBytes > 0) rank -= (cumulative >> ((smallerBytes - 1) * 8)) & 0xFF;
		assert(rank >= 0 && rank < 8);
		//spread the 1's from interesting byte to each byte
		//first put every pair of bits into each 2-byte boundary
		//then select only those pairs
		//then spread the pairs into each byte boundary
		//and select the ones
		uint64_t spreadBits = (((interestingByte * 0x0000040010004001) & 0x0003000300030003) * 0x0000000000000081) & 0x0101010101010101;
/*
0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  abcd efgh
0000 0000  0000 00ab  cdef gh00  0000 abcd  efgh 0000  00ab cdef  gh00 0000  abcd efgh  * 0x0000040010004001
0000 0000  0000 00ab  0000 0000  0000 00cd  0000 0000  0000 00ef  0000 0000  0000 00gh  & 0x0003000300030003
0000 000a  b000 00ab  0000 000c  d000 00cd  0000 000e  f000 00ef  0000 000g  h000 00gh  * 0x0000000000000081
0000 000a  0000 000b  0000 000c  0000 000d  0000 000e  0000 000f  0000 000g  0000 000h  & 0x0101010101010101
*/
		//find the position from the bits the same way as from the bytes
		uint64_t cumulativeBits = spreadBits * PrefixSumMultiplierConstant;
		uint64_t bitRankFinder = ((rank + 1) & 0xFF) * PrefixSumMultiplierConstant;
		uint64_t bitRankMask = (cumulativeBits | SignMask) - bitRankFinder;
		int smallerBits = 8 - ((((bitRankMask & SignMask) >> 7) * PrefixSumMultiplierConstant) >> 56);
		assert(smallerBits >= 0);
		assert(smallerBits < 8);
		return smallerBytes * 8 + smallerBits;
	}

	static uint64_t MortonHigh(uint64_t left, uint64_t right)
	{
		return Interleave(left >> 32, right >> 32);
	}

	static uint64_t MortonLow(uint64_t left, uint64_t right)
	{
		return Interleave(left & 0xFFFFFFFF, right & 0xFFFFFFFF);
	}

	//http://graphics.stanford.edu/~seander/bithacks.html#InterleaveBMN
	static uint64_t Interleave(uint64_t x, uint64_t y)
	{
		assert(x == (x & 0xFFFFFFFF));
		assert(y == (y & 0xFFFFFFFF));
		static const uint64_t B[] = {0x5555555555555555, 0x3333333333333333, 0x0F0F0F0F0F0F0F0F, 0x00FF00FF00FF00FF, 0x0000FFFF0000FFFF};
		static const uint64_t S[] = {1, 2, 4, 8, 16};

		x = (x | (x << S[4])) & B[4];
		x = (x | (x << S[3])) & B[3];
		x = (x | (x << S[2])) & B[2];
		x = (x | (x << S[1])) & B[1];
		x = (x | (x << S[0])) & B[0];

		y = (y | (y << S[4])) & B[4];
		y = (y | (y << S[3])) & B[3];
		y = (y | (y << S[2])) & B[2];
		y = (y | (y << S[1])) & B[1];
		y = (y | (y << S[0])) & B[0];

		return x | (y << 1);
	}
};

class RowConfirmation
{
public:
	RowConfirmation(char rows, bool partial) : rows(rows), partial(partial)
#ifdef EXTRACORRECTNESSASSERTIONS
	,exists(0)
#endif
	{};
	char rows;
	bool partial;
#ifdef EXTRACORRECTNESSASSERTIONS
	Word exists;
#endif
	bool operator>(const RowConfirmation& other) const
	{
		return rows > other.rows || (rows == other.rows && partial && !other.partial);
	}
	bool operator<(const RowConfirmation& other) const
	{
		return rows < other.rows || (rows == other.rows && !partial && other.partial);
	}
	bool operator==(const RowConfirmation& other) const
	{
		return rows == other.rows && partial == other.partial;
	}
	bool operator!=(const RowConfirmation& other) const
	{
		return !(*this == other);
	}
	bool operator>=(const RowConfirmation& other) const
	{
		return !(*this < other);
	}
	bool operator<=(const RowConfirmation& other) const
	{
		return !(*this > other);
	}
};

template <typename LengthType, typename ScoreType, typename Word>
class WordSlice
{
public:
	WordSlice() :
	VP(0),
	VN(0),
	scoreEnd(0),
	scoreBeforeStart(0),
	confirmedRows(0, false),
	scoreBeforeExists(false),
	scoreEndExists(true)
	{}
	WordSlice(Word VP, Word VN, ScoreType scoreEnd, ScoreType scoreBeforeStart, int confirmedRows, bool scoreBeforeExists) :
	VP(VP),
	VN(VN),
	scoreEnd(scoreEnd),
	scoreBeforeStart(scoreBeforeStart),
	confirmedRows(confirmedRows, false),
	scoreBeforeExists(scoreBeforeExists),
	scoreEndExists(true)
	{}
	Word VP;
	Word VN;
	ScoreType scoreEnd;
	ScoreType scoreBeforeStart;
	RowConfirmation confirmedRows;
	bool scoreBeforeExists;
	bool scoreEndExists;

	WordSlice mergeWith(const WordSlice& other) const
	{
		auto result = mergeTwoSlices(*this, other);
		return mergeTwoSlices(*this, other);
	}

#ifdef EXTRACORRECTNESSASSERTIONS

	bool cellExists(int row) const
	{
		return confirmedRows.exists & (((Word)1) << row);
	}

	ScoreType getValueIfExists(int row, ScoreType defaultValue) const
	{
		if (cellExists(row)) return getValue(row);
		return defaultValue;
	}

#endif

	ScoreType getValue(int row) const
	{
		auto mask = WordConfiguration<Word>::AllOnes;
		if (row < WordConfiguration<Word>::WordSize-1) mask = ~(WordConfiguration<Word>::AllOnes << (row + 1));
		auto value = scoreBeforeStart + WordConfiguration<Word>::popcount(VP & mask) - WordConfiguration<Word>::popcount(VN & mask);
		return value;
	}

	void setValue(int row, ScoreType value)
	{
#ifdef EXTRACORRECTNESSASSERTIONS
		confirmedRows.exists |= ((Word)1) << row;
#endif
		if (!confirmedRows.partial)
		{
			confirmedRows.partial = true;
			scoreBeforeStart = value + row + 1;
			if (row < WordConfiguration<Word>::WordSize-1)
			{
				VN = ~(WordConfiguration<Word>::AllOnes << (row + 1));
				VP = WordConfiguration<Word>::AllOnes << (row + 1);
			}
			else
			{
				VN = WordConfiguration<Word>::AllOnes;
				VP = WordConfiguration<Word>::AllZeros;
			}
			confirmedRows.rows = row;
			scoreEnd = value + WordConfiguration<Word>::WordSize - row - 1;
			return;
		}
		assert(confirmedRows.rows < row);
		if (confirmedRows.rows == row - 1)
		{
			auto oldscore = scoreEnd - (WordConfiguration<Word>::WordSize - confirmedRows.rows - 1);
			assert(oldscore == getValue(confirmedRows.rows));
			assert(value >= oldscore - 1);
			assert(value <= oldscore + 1);
			Word mask = ((Word)1) << row;
			switch (value + 1 - oldscore)
			{
				case 0:
					VN |= mask;
					VP &= ~mask;
					scoreEnd -= 2;
					break;
				case 1:
					VN &= ~mask;
					VP &= ~mask;
					scoreEnd--;
					break;
				case 2:
					VP |= mask;
					VN &= ~mask;
					break;
			}
			confirmedRows.rows = row;
			return;
		}
		ScoreType scores[WordConfiguration<Word>::WordSize];
		scores[0] = scoreBeforeStart + (VP & 1) - (VN & 1);
		for (int i = 1; i <= confirmedRows.rows; i++)
		{
			auto mask = ((Word)1) << i;
			scores[i] = scores[i-1] + ((VP & mask) ? 1 : 0) - ((VN & mask) ? 1 : 0);
		}
		for (int i = confirmedRows.rows+1; i <= row; i++)
		{
			scores[i] = scores[i-1] + 1;
		}
		for (int i = 0; i <= row; i++)
		{
			scores[i] = std::min(scores[i], value + row - i);
		}
		assert(scores[0] >= scoreBeforeStart - 1);
		assert(scores[0] <= scoreBeforeStart + 1);
		switch(scores[0] + 1 - scoreBeforeStart)
		{
			case 0:
				VP &= ~(Word)1;
				VN |= 1;
				break;
			case 1:
				VP &= ~(Word)1;
				VN &= ~(Word)1;
				break;
			case 2:
				VP |= 1;
				VN &= ~(Word)1;
				break;
		}
		for (int i = 1; i <= row; i++)
		{
			assert(scores[i] >= scores[i-1] - 1);
			assert(scores[i] <= scores[i-1] + 1);
			Word mask = ((Word)1) << i;
			switch(scores[i] + 1 - scores[i-1])
			{
				case 0:
					VP &= ~mask;
					VN |= mask;
					break;
				case 1:
					VP &= ~mask;
					VN &= ~mask;
					break;
				case 2:
					VP |= mask;
					VN &= ~mask;
					break;
			}
		}
		scoreEnd = scores[row] + WordConfiguration<Word>::WordSize - 1 - row;
		confirmedRows.rows = row;
	}

private:


	static uint64_t bytePrefixSums(uint64_t value, int addition)
	{
		value <<= WordConfiguration<Word>::ChunkBits;
		assert(addition >= 0);
		value += addition;
		return value * WordConfiguration<Word>::PrefixSumMultiplierConstant;
	}

	static uint64_t byteVPVNSum(uint64_t prefixSumVP, uint64_t prefixSumVN)
	{
		uint64_t result = WordConfiguration<Word>::SignMask;
		assert((prefixSumVP & result) == 0);
		assert((prefixSumVN & result) == 0);
		result += prefixSumVP;
		result -= prefixSumVN;
		result ^= WordConfiguration<Word>::SignMask;
		return result;
	}

	static WordSlice mergeTwoSlices(WordSlice left, WordSlice right)
	{
		//O(log w), because prefix sums need log w chunks of log w bits
		static_assert(std::is_same<Word, uint64_t>::value);
#ifdef EXTRABITVECTORASSERTIONS
		auto correctValue = mergeTwoSlicesCellByCell(left, right);
#endif
		if (left.scoreBeforeStart > right.scoreBeforeStart) std::swap(left, right);
		auto newConfirmedRows = confirmedRowsInMerged(left, right);
#ifdef EXTRACORRECTNESSASSERTIONS
		assert(newConfirmedRows == confirmedRowsInMergedCellByCell(left, right));
#endif
		WordSlice result;
		assert((left.VP & left.VN) == WordConfiguration<Word>::AllZeros);
		assert((right.VP & right.VN) == WordConfiguration<Word>::AllZeros);
		auto masks = differenceMasks(left.VP, left.VN, right.VP, right.VN, right.scoreBeforeStart - left.scoreBeforeStart);
		auto leftSmaller = masks.first;
		auto rightSmaller = masks.second;
		assert((leftSmaller & rightSmaller) == 0);
		auto mask = (rightSmaller | ((leftSmaller | rightSmaller) - (rightSmaller << 1))) & ~leftSmaller;
		uint64_t leftReduction = leftSmaller & (rightSmaller << 1);
		uint64_t rightReduction = rightSmaller & (leftSmaller << 1);
		if ((rightSmaller & 1) && left.scoreBeforeStart < right.scoreBeforeStart)
		{
			rightReduction |= 1;
		}
		assert((leftReduction & right.VP) == leftReduction);
		assert((rightReduction & left.VP) == rightReduction);
		assert((leftReduction & left.VN) == leftReduction);
		assert((rightReduction & right.VN) == rightReduction);
		left.VN &= ~leftReduction;
		right.VN &= ~rightReduction;
		result.VN = (left.VN & ~mask) | (right.VN & mask);
		result.VP = (left.VP & ~mask) | (right.VP & mask);
		assert((result.VP & result.VN) == 0);
		result.scoreBeforeStart = std::min(left.scoreBeforeStart, right.scoreBeforeStart);
		result.scoreEnd = std::min(left.scoreEnd, right.scoreEnd);
		if (left.scoreBeforeStart < right.scoreBeforeStart)
		{
			result.scoreBeforeExists = left.scoreBeforeExists;
		}
		else if (right.scoreBeforeStart < left.scoreBeforeStart)
		{
			result.scoreBeforeExists = right.scoreBeforeExists;
		}
		else
		{
			result.scoreBeforeExists = left.scoreBeforeExists || right.scoreBeforeExists;
		}
		result.confirmedRows = newConfirmedRows;
		assert(result.confirmedRows >= std::min(left.confirmedRows, right.confirmedRows));
		assert(result.confirmedRows <= std::max(left.confirmedRows, right.confirmedRows));
		assert(result.scoreEnd == result.scoreBeforeStart + WordConfiguration<Word>::popcount(result.VP) - WordConfiguration<Word>::popcount(result.VN));
#ifdef EXTRABITVECTORASSERTIONS
		assert(result.VP == correctValue.VP);
		assert(result.VN == correctValue.VN);
		assert(result.scoreBeforeStart == correctValue.scoreBeforeStart);
		assert(result.scoreEnd == correctValue.scoreEnd);
#endif
		return result;
	}

	static RowConfirmation confirmedRowsInMerged(WordSlice left, WordSlice right)
	{
		if (left.confirmedRows == right.confirmedRows) return left.confirmedRows;
		if (right.confirmedRows > left.confirmedRows) std::swap(left, right);
		assert(right.confirmedRows < left.confirmedRows);
		ScoreType leftScore = left.scoreBeforeStart;
		ScoreType rightScore = right.scoreBeforeStart;
		Word confirmedMask = ~(WordConfiguration<Word>::AllOnes << right.confirmedRows.rows);
		leftScore += WordConfiguration<Word>::popcount(left.VP & confirmedMask);
		leftScore -= WordConfiguration<Word>::popcount(left.VN & confirmedMask);
		rightScore += WordConfiguration<Word>::popcount(right.VP & confirmedMask);
		rightScore -= WordConfiguration<Word>::popcount(right.VN & confirmedMask);
		if (right.confirmedRows.rows == left.confirmedRows.rows)
		{
			assert(!right.confirmedRows.partial);
			assert(left.confirmedRows.partial);
			auto mask = ((Word)1) << left.confirmedRows.rows;
			rightScore -= 1;
			if (left.VP & mask)
			{
				return { left.confirmedRows.rows, leftScore <= rightScore };
			}
			else
			{
				leftScore -= 1;
				return { left.confirmedRows.rows, leftScore <= rightScore };
			}
		}
		Word premask = ((Word)1) << right.confirmedRows.rows;
		leftScore += (left.VP & premask) ? 1 : 0;
		leftScore -= (left.VN & premask) ? 1 : 0;
		if (right.confirmedRows.partial && (right.VP & premask))
		{
		}
		else
		{
			rightScore -= 1;
		}
		if (leftScore == rightScore + 1)
		{
			return { right.confirmedRows.rows, true };
		}
		if (leftScore > rightScore + 1)
		{
			return right.confirmedRows;
		}
		if (left.confirmedRows.rows > right.confirmedRows.rows + 1)
		{
			Word partiallyConfirmedMask = 0;
			if (left.confirmedRows.rows < WordConfiguration<Word>::WordSize)
			{
				partiallyConfirmedMask = WordConfiguration<Word>::AllOnes << left.confirmedRows.rows;
			}
			partiallyConfirmedMask = ~partiallyConfirmedMask;
			assert(right.confirmedRows.rows + 1 < WordConfiguration<Word>::WordSize);
			partiallyConfirmedMask &= WordConfiguration<Word>::AllOnes << (right.confirmedRows.rows + 1);
			Word low = left.VP & partiallyConfirmedMask;
			Word high = ~left.VN & partiallyConfirmedMask;
			Word mortonLow = WordConfiguration<Word>::MortonLow(low, high);
			Word mortonHigh = WordConfiguration<Word>::MortonHigh(low, high);
			assert(leftScore <= rightScore);
			auto pos = WordConfiguration<Word>::BitPosition(mortonLow, mortonHigh, rightScore - leftScore);
			if (pos/2 < left.confirmedRows.rows)
			{
				auto nextpos = WordConfiguration<Word>::BitPosition(mortonLow, mortonHigh, rightScore - leftScore + 1);
				return { pos/2, nextpos/2 > pos/2 };
			}
			leftScore += WordConfiguration<Word>::popcount(left.VP & partiallyConfirmedMask);
			leftScore -= WordConfiguration<Word>::popcount(left.VN & partiallyConfirmedMask);
			rightScore -= left.confirmedRows.rows - right.confirmedRows.rows - 1;
		}
		if (!left.confirmedRows.partial) return left.confirmedRows;
		assert(left.confirmedRows.partial);
		assert(left.confirmedRows.rows < WordConfiguration<Word>::WordSize);
		Word postmask = ((Word)1) << left.confirmedRows.rows;
		rightScore -= 1;
		if (left.VP & postmask)
		{
			if (leftScore <= rightScore) return left.confirmedRows;
		}
		else
		{
			leftScore -= 1;
			assert(leftScore <= rightScore);
			return left.confirmedRows;
		}
		return { left.confirmedRows.rows, false };
	}

	static std::pair<uint64_t, uint64_t> differenceMasks(uint64_t leftVP, uint64_t leftVN, uint64_t rightVP, uint64_t rightVN, int scoreDifference)
	{
#ifdef EXTRABITVECTORASSERTIONS
		auto correctValue = differenceMasksCellByCell(leftVP, leftVN, rightVP, rightVN, scoreDifference);
#endif
		assert(scoreDifference >= 0);
		const uint64_t signmask = WordConfiguration<Word>::SignMask;
		const uint64_t lsbmask = WordConfiguration<Word>::LSBMask;
		const int chunksize = WordConfiguration<Word>::ChunkBits;
		const uint64_t allones = WordConfiguration<Word>::AllOnes;
		const uint64_t allzeros = WordConfiguration<Word>::AllZeros;
		uint64_t VPcommon = ~(leftVP & rightVP);
		uint64_t VNcommon = ~(leftVN & rightVN);
		leftVP &= VPcommon;
		leftVN &= VNcommon;
		rightVP &= VPcommon;
		rightVN &= VNcommon;
		//left is lower everywhere
		if (scoreDifference > WordConfiguration<Word>::popcount(rightVN) + WordConfiguration<Word>::popcount(leftVP))
		{
			return std::make_pair(allones, allzeros);
		}
		if (scoreDifference == 128 && rightVN == allones && leftVP == allones)
		{
			return std::make_pair(allones ^ ((Word)1 << (WordConfiguration<Word>::WordSize-1)), allzeros);
		}
		else if (scoreDifference == 0 && rightVN == allones && leftVP == allones)
		{
			return std::make_pair(0, allones);
		}
		assert(scoreDifference >= 0);
		assert(scoreDifference < 128);
		uint64_t byteVPVNSumLeft = byteVPVNSum(bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(leftVP), 0), bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(leftVN), 0));
		uint64_t byteVPVNSumRight = byteVPVNSum(bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(rightVP), scoreDifference), bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(rightVN), 0));
		uint64_t difference = byteVPVNSumLeft;
		{
			//take the bytvpvnsumright and split it from positive/negative values into two vectors with positive values, one which needs to be added and the other deducted
			//smearmask is 1 where the number needs to be deducted, and 0 where it needs to be added
			//except sign bits which are all 0
			uint64_t smearmask = ((byteVPVNSumRight & signmask) >> (chunksize-1)) * ((((Word)1) << (chunksize-1))-1);
			assert((smearmask & signmask) == 0);
			uint64_t deductions = ~smearmask & byteVPVNSumRight & ~signmask;
			//byteVPVNSumRight is in one's complement so take the not-value + 1
			uint64_t additions = (smearmask & ~byteVPVNSumRight) + (smearmask & lsbmask);
			assert((deductions & signmask) == 0);
			uint64_t signsBefore = difference & signmask;
			//unset the sign bits so additions don't interfere with other chunks
			difference &= ~signmask;
			difference += additions;
			//the sign bit is 1 if the value went from <0 to >=0
			//so in that case we need to flip it
			difference ^= signsBefore;
			signsBefore = difference & signmask;
			//set the sign bits so that deductions don't interfere with other chunks
			difference |= signmask;
			difference -= deductions;
			//sign bit is 0 if the value went from >=0 to <0
			//so flip them to the correct values
			signsBefore ^= signmask & ~difference;
			difference &= ~signmask;
			difference |= signsBefore;
		}
		//difference now contains the prefix sum difference (left-right) at each chunk
		uint64_t resultLeftSmallerThanRight = 0;
		uint64_t resultRightSmallerThanLeft = 0;
		for (int bit = 0; bit < chunksize; bit++)
		{
			uint64_t signsBefore = difference & signmask;
			//unset the sign bits so additions don't interfere with other chunks
			difference &= ~signmask;
			difference += leftVP & lsbmask;
			difference += rightVN & lsbmask;
			//the sign bit is 1 if the value went from <0 to >=0
			//so in that case we need to flip it
			difference ^= signsBefore;
			signsBefore = difference & signmask;
			//set the sign bits so that deductions don't interfere with other chunks
			difference |= signmask;
			difference -= leftVN & lsbmask;
			difference -= rightVP & lsbmask;
			//sign bit is 0 if the value went from >=0 to <0
			//so flip them to the correct values
			signsBefore ^= signmask & ~difference;
			difference &= ~signmask;
			difference |= signsBefore;
			leftVN >>= 1;
			leftVP >>= 1;
			rightVN >>= 1;
			rightVP >>= 1;
			//difference now contains the prefix sums difference (left-right) at each byte at (bit)'th bit
			//left < right when the prefix sum difference is negative (sign bit is set)
			uint64_t negative = (difference & signmask);
			resultLeftSmallerThanRight |= negative >> (WordConfiguration<Word>::ChunkBits - 1 - bit);
			//Test equality to zero. If it's zero, substracting one will make the sign bit 0, otherwise 1
			uint64_t notEqualToZero = ((difference | signmask) - lsbmask) & signmask;
			//right > left when the prefix sum difference is positive (not zero and not negative)
			resultRightSmallerThanLeft |= (notEqualToZero & ~negative) >> (WordConfiguration<Word>::ChunkBits - 1 - bit);
		}
#ifdef EXTRABITVECTORASSERTIONS
		assert(resultLeftSmallerThanRight == correctValue.first);
		assert(resultRightSmallerThanLeft == correctValue.second);
#endif
		return std::make_pair(resultLeftSmallerThanRight, resultRightSmallerThanLeft);
	}

};

#endif