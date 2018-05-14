#include "Sh3BinaryEvaluator.h"
#include <cryptoTools/Common/Matrix.h>
#include <cryptoTools/Common/Log.h>
#include <libOTe/Tools/Tools.h>
#include <cryptoTools/Crypto/sha1.h>
#include "Sh3Converter.h"
#include <immintrin.h>


//std::ostream& operator<<(std::ostream& out, const __m256i& block);
//namespace osuCrypto
//{
//    using ::operator<<;
//}

inline bool eq(const __m256i& lhs, const __m256i& rhs)
{
    __m256i zero = _mm256_xor_si256(lhs, lhs);
    __m256i neq = _mm256_xor_si256(lhs, rhs);
    return memcmp(&zero, &neq, 32) == 0;
}

inline bool neq(const __m256i& lhs, const __m256i& rhs)
{
    return !eq(lhs, rhs);
}

#ifdef _MSC_VER

inline __m256i operator^(const __m256i& lhs, const __m256i& rhs)
{
    return _mm256_xor_si256(lhs, rhs);
}
inline __m256i operator&(const __m256i& lhs, const __m256i& rhs)
{
    return _mm256_and_si256(lhs, rhs);
}

#endif

namespace aby3
{



    using namespace oc;

    void Sh3BinaryEvaluator::setCir(BetaCircuit * cir, u64 width)
    {


        if (cir->mLevelCounts.size() == 0)
            cir->levelByAndDepth();

        //if (cir->mInputs.size() != 2) throw std::runtime_error(LOCATION);

        mCir = cir;
        //auto bits = sizeof(i64) * 8;
        //auto simdWidth = (width + bits - 1) / bits;

        // each row of mem corresponds to a wire. Each column of mem corresponds to 64 SIMD bits
        mMem.resize(width, mCir->mWireCount, 8);
        //mMem[0].resize(cir->mWireCount, simdWidth);
        //mMem[1].resize(cir->mWireCount, simdWidth);
        //mMem[0].setZero();
        //mMem[1].setZero();


#ifdef BINARY_ENGINE_DEBUG
        if (mDebug)
        {
            mPlainWires_DEBUG.resize(width);
            for (auto& m : mPlainWires_DEBUG)
            {
                m.resize(cir->mWireCount);
                memset(m.data(), 0, m.size() * sizeof(DEBUG_Triple));
            }
        }
#endif
    }

    void Sh3BinaryEvaluator::setReplicatedInput(u64 idx, const Sh3::sbMatrix & in)
    {
        mLevel = 0;
        auto& inWires = mCir->mInputs[idx].mWires;
        auto simdWidth = mMem.simdWidth();
        auto bitCount = in.bitCount();

        if (mCir == nullptr)
            throw std::runtime_error(LOCATION);
        if (idx >= mCir->mInputs.size())
            throw std::invalid_argument("input index out of bounds");
        if (bitCount != inWires.size())
            throw std::invalid_argument("input data wrong size");
        if (in.rows() != 1)
            throw std::invalid_argument("incorrect number of simd rows");


        std::array<char, 2> vals{ 0,~0 };

        for (u64 i = 0; i < 2; ++i)
        {
            auto& shares = mMem.mShares[i];
            BitIterator iter((u8*)in.mShares[i].data(), 0);

            for (u64 j = 0; j < inWires.size(); ++j, ++iter)
            {
                if (shares.rows() <= inWires[j])
                    throw std::runtime_error(LOCATION);

                char v = vals[*iter];
                memset(shares.data() + simdWidth * inWires[j], v, simdWidth * sizeof(block_type));
            }
        }


#ifdef BINARY_ENGINE_DEBUG
        //throw std::runtime_error("");
        if (mDebug)
        {
            auto prevIdx = (mDebugPartyIdx + 2) % 3;
            auto bv0 = BitVector((u8*)in.mShares[0][0].data(), inWires.size());
            auto bv1 = BitVector((u8*)in.mShares[1][0].data(), inWires.size());

            for (u64 i = 0; i < inWires.size(); ++i)
            {
                for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
                {
                    auto& m = mPlainWires_DEBUG[r];
                    m[inWires[i]].mBits[mDebugPartyIdx] = bv0[i];
                    m[inWires[i]].mBits[prevIdx] = bv1[i];
                    m[inWires[i]].mIsSet = true;
                    //if (inWires[i] < 10)
                    //	ostreamLock(std::cout) << mPartyIdx << " w[" << inWires[i] << "] = "
                    //	<< (int)m[inWires[i]].mBits[mPartyIdx] << std::endl;
                }

                validateWire(inWires[i]);
            }
        }
#endif
    }

    void Sh3BinaryEvaluator::setInput(u64 idx, const Sh3::sbMatrix  & in)
    {
        mLevel = 0;
        auto& inWires = mCir->mInputs[idx].mWires;
        auto simdWidth = mMem.simdWidth();

        auto bitCount = in.bitCount();
        auto inRows = in.rows();

        if (mCir == nullptr)
            throw std::runtime_error(LOCATION);
        if (idx >= mCir->mInputs.size())
            throw std::invalid_argument("input index out of bounds");
        if (bitCount != inWires.size())
            throw std::invalid_argument("input data wrong size");
        if (inRows != mMem.shareCount())
            throw std::invalid_argument("incorrect number of rows");

        for (u64 i = 0; i < inWires.size() - 1; ++i)
        {
            if (inWires[i] + 1 != inWires[i + 1])
                throw std::runtime_error("expecting contiguous input wires. " LOCATION);
        }

        for (u64 i = 0; i < 2; ++i)
        {
            auto& shares = mMem.mShares[i];

            MatrixView<u8> inView(
                (u8*)(in.mShares[i].data()),
                (u8*)(in.mShares[i].data() + in.mShares[i].size()),
                sizeof(i64) * in.mShares[i].cols());

            if (inWires.back() > shares.rows())
                throw std::runtime_error(LOCATION);

            MatrixView<u8> memView(
                (u8*)(shares.data() + simdWidth * inWires.front()),
                (u8*)(shares.data() + simdWidth * (inWires.back() + 1)),
                sizeof(block_type) * simdWidth);

            if (memView.data() + memView.size() > (u8*)(shares.data() + shares.size()))
                throw std::runtime_error(LOCATION);

            sse_transpose(inView, memView);
            //std::cout << " in* " << std::endl;
            //for (u64 r = 0; r < inView.bounds()[0]; ++r)
            //{
            //	BitVector bv(inView[r].data(), inView[r].size() * 8);
            //	std::cout << bv;
            //	//for (u64 c = 0; c < inView.bounds()[1]; ++c)
            //	//{
            //	//	std::cout << std::hex << (u64)inView(r, c) << " ";
            //	//}
            //	std::cout << std::endl;
            //}
            //std::cout << std::endl;
            //
            //
            //std::cout << " out*" << std::endl;
            //for (u64 r = 0; r < memView.bounds()[0]; ++r)
            //{
            //	for (u64 c = 0; c < memView.bounds()[1]; ++c)
            //	{
            //		std::cout << std::hex << (u64)memView(r, c) << " ";
            //	}
            //	std::cout << std::endl;
            //}
            //std::cout << std::endl;
        }

#ifdef BINARY_ENGINE_DEBUG

        if (mDebug)
        {
            //Sh3Converter convt;
            //Sh3::sPackedBin pack;
            //convt.toPackedBin(in, pack);
            //pack.trim();
            //mMem.trim();
            //for (u64 i = 0; i < inWires.size(); ++i)
            //{
            //    for (u64 j = 0; j < 2; ++j)
            //    {
            //        auto pPtr = pack.mShares[j][i].data();
            //        auto mPtr = mMem.mShares[j][inWires[i]].data();
            //        if (memcmp(pPtr, mPtr, pack.mShares[j].cols() * sizeof(i64)))
            //        {

            //            BitVector a((u8*)pPtr, pack.shareCount());
            //            BitVector b((u8*)mPtr, mMem.shareCount());

            //            auto leftover = ((pack.shareCount() + 63) / 64) * 64 - pack.shareCount();
            //            BitVector aa, bb;

            //            aa.append((u8*)pPtr, leftover, pack.shareCount());
            //            bb.append((u8*)mPtr, leftover, pack.shareCount());
            //            ostreamLock(std::cout)
            //                << j << std::endl << std::hex
            //                //<< pack.mShares[j].row(i) << std::endl
            //                //<< mMem.mShares[j].row(inWires[i]) << std::endl
            //                << a << " " << aa << std::endl
            //                << b << " " << bb << std::endl;
            //            throw std::runtime_error("");
            //        }
            //    }
            //}

            for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
            {



                auto prevIdx = (mDebugPartyIdx + 2) % 3;
                auto& m = mPlainWires_DEBUG[r];
                auto bv0 = BitVector((u8*)in.mShares[0][r].data(), inWires.size());
                auto bv1 = BitVector((u8*)in.mShares[1][r].data(), inWires.size());

                for (u64 i = 0; i < inWires.size(); ++i)
                {
                    m[inWires[i]].mBits[mDebugPartyIdx] = bv0[i];
                    m[inWires[i]].mBits[prevIdx] = bv1[i];
                    m[inWires[i]].mIsSet = true;
                    //if (inWires[i] < 10)
                    //	ostreamLock(std::cout) << mPartyIdx << " w[" << inWires[i] << "] = "
                    //	<< (int)m[inWires[i]].mBits[mPartyIdx] << std::endl;
                }
            }
        }
        //if (mDebugPartyIdx == 0)
        //    for (u64 i = 0; i < inWires.size(); ++i)
        //    {
        //        validateWire(inWires[i]);
        //    }
        //Matrix nn(in);
        //getOutput(inWires, nn);

        //if (nn.mShares[0] != in.mShares[0])
        //{
        //	std::cout << "exp " << in.mShares[0] << std::endl;
        //	std::cout << "act " << nn.mShares[0] << std::endl;
        //	throw std::runtime_error(LOCATION);
        //}
        //if (nn.mShares[1] != in.mShares[1]) throw std::runtime_error(LOCATION);
#endif

    }

    void Sh3BinaryEvaluator::setInput(u64 idx, const Sh3::sPackedBin & in)
    {
        //auto simdWidth = mMem.simdWidth();

        if (mCir == nullptr)
            throw std::runtime_error(LOCATION);
        if (idx >= mCir->mInputs.size())
            throw std::invalid_argument("input index out of bounds");
        if (in.shareCount() != mMem.shareCount())
            throw std::runtime_error(LOCATION);
        if (in.bitCount() != mCir->mInputs[idx].mWires.size())
            throw std::runtime_error(LOCATION);

        auto& inWires = mCir->mInputs[idx];
        for (u64 i = 0; i < 2; ++i)
        {
            auto& share = mMem.mShares[i];
            for (u64 j = 0; j < inWires.size(); ++j)
            {
                memcpy(
                    share.data() + mMem.simdWidth() * inWires[j],
                    in.mShares[i].data() + in.simdWidth() * j,
                    in.simdWidth() * sizeof(i64));
            }
        }

#ifdef BINARY_ENGINE_DEBUG
        if (mDebug)
        {
            throw std::runtime_error("");
            for (u64 i = 0; i < inWires.size(); ++i)
            {
                auto shareCount = mPlainWires_DEBUG.size();

                auto prevIdx = (mDebugPartyIdx + 2) % 3;
                //auto& m = mPlainWires_DEBUG[r];
                auto bv0 = BitVector((u8*)in.mShares[0][i].data(), shareCount);
                auto bv1 = BitVector((u8*)in.mShares[1][i].data(), shareCount);

                for (u64 r = 0; r < shareCount; ++r)
                {
                    auto& triple = mPlainWires_DEBUG[r][inWires[i]];

                    triple.mBits[mDebugPartyIdx] = bv0[r];
                    triple.mBits[prevIdx] = bv1[r];
                    triple.mIsSet = true;
                }

                validateWire(inWires[i]);
            }
        }
#endif
    }

#ifdef BINARY_ENGINE_DEBUG
    void Sh3BinaryEvaluator::validateMemory()
    {
        for (u64 i = 0; i < mCir->mWireCount; ++i)
        {
            validateWire(i);
        }
    }


    void Sh3BinaryEvaluator::validateWire(u64 wireIdx)
    {
        if (mDebug && mPlainWires_DEBUG[0][wireIdx].mIsSet)
        {
            auto shareCount = mPlainWires_DEBUG.size();
            auto prevIdx = (mDebugPartyIdx + 2) % 3;

            for (u64 r = 0; r < shareCount; ++r)
            {
                auto& triple = mPlainWires_DEBUG[r][wireIdx];

                auto bit0 = extractBitShare(r, wireIdx, 0);
                auto bit1 = extractBitShare(r, wireIdx, 1);
                if (triple.mBits[mDebugPartyIdx] != bit0)
                {
                    std::cout << "party " << mDebugPartyIdx << " wire " << wireIdx << " row " << r << " s " << 0 << " ~~"
                        << " exp:" << int(triple.mBits[mDebugPartyIdx])
                        << " act:" << int(bit0) << std::endl;

                    throw std::runtime_error(LOCATION);

                }
                if (triple.mBits[prevIdx] != bit1)
                {

                    std::cout << "party " << mDebugPartyIdx << " wire " << wireIdx << " row " << r << " s " << 1 << " ~~"
                        << " exp:" << int(triple.mBits[prevIdx])
                        << " act:" << int(bit1) << std::endl;
                    throw std::runtime_error(LOCATION);
                }
            }
        }
    }
    void Sh3BinaryEvaluator::distributeInputs()
    {

        if (mDebug)
        {

            auto prevIdx = (mDebugPartyIdx + 2) % 3;
            auto nextIdx = (mDebugPartyIdx + 1) % 3;
            for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
            {
                auto& m = mPlainWires_DEBUG[r];

                std::vector<u8> s0(m.size()), s1;
                for (u64 i = 0; i < s0.size(); ++i)
                {
                    s0[i] = m[i].mBits[mDebugPartyIdx];
                }
                mDebugPrev.asyncSendCopy(s0);
                mDebugNext.asyncSendCopy(s0);
                mDebugPrev.recv(s0);
                mDebugNext.recv(s1);
                if (s0.size() != m.size())
                    throw std::runtime_error(LOCATION);

                for (u64 i = 0; i < m.size(); ++i)
                {
                    if (m[i].mBits[prevIdx] != s0[i])
                        throw std::runtime_error(LOCATION);

                    m[i].mBits[nextIdx] = s1[i];
                }
            }

            //block b = hashDebugState();
            //ostreamLock(std::cout) << "b" << mDebugPartyIdx << " " << b << std::endl;
        }
    }
    oc::block Sh3BinaryEvaluator::hashDebugState()
    {
        if (mDebug == false)
            return ZeroBlock;

        SHA1 sha(sizeof(block));
        for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
        {
            auto& m = mPlainWires_DEBUG[r];
            sha.Update(m.data(), m.size());
        }

        block b;
        sha.Final(b);
        return b;
    }
#endif


    Sh3Task Sh3BinaryEvaluator::asyncEvaluate(
        Sh3Task dep,
        oc::BetaCircuit * cir,
        std::vector<const Sh3::sbMatrix*> inputs,
        std::vector<Sh3::sbMatrix*> outputs)
    {
        if (cir->mInputs.size() != inputs.size())
            throw std::runtime_error(LOCATION);
        if (cir->mOutputs.size() != outputs.size())
            throw std::runtime_error(LOCATION);

        return dep.then([this, cir, inputs = std::move(inputs)](Sh3::CommPkg& comm, Sh3Task& self)
        {
            auto width = inputs[0]->rows();
            setCir(cir, width);

            for (u64 i = 0; i < inputs.size(); ++i)
            {
                if (inputs[i]->rows() != width)
                    throw std::runtime_error(LOCATION);

                setInput(i, *inputs[i]);
            }

#ifdef BINARY_ENGINE_DEBUG
            validateMemory();
            distributeInputs();
#endif

            roundCallback(comm, self);

        }).then([this, outputs = std::move(outputs)](Sh3Task& self)
        {
            for (u64 i = 0; i < outputs.size(); ++i)
            {
                getOutput(i, *outputs[i]);
            }
        });
    }

    Sh3Task Sh3BinaryEvaluator::asyncEvaluate(Sh3Task dependency)
    {
#ifdef BINARY_ENGINE_DEBUG
        validateMemory();
        distributeInputs();
#endif

        return dependency.then([this](Sh3::CommPkg& comm, Sh3Task& self)
        {
            roundCallback(comm, self);
        });
    }

    u8 extractBit(i64* data, u64 rowIdx)
    {
        auto k = rowIdx / (sizeof(i64) * 8);
        auto j = rowIdx % (sizeof(i64) * 8);

        return (data[k] >> j) & 1;
    }

    u8 Sh3BinaryEvaluator::extractBitShare(u64 rowIdx, u64 wireIdx, u64 shareIdx)
    {
        if (rowIdx >= mMem.shareCount())
            throw std::runtime_error("");

        auto row = (i64*)mMem.mShares[shareIdx][wireIdx].data();
        auto k = rowIdx / (sizeof(i64) * 8);
        auto j = rowIdx % (sizeof(i64) * 8);

        return (row[k] >> j) & 1;
    }


    std::ostream& operator<<(std::ostream& out, Sh3BinaryEvaluator::DEBUG_Triple& triple)
    {
        out << "(" << (int) triple.mBits[0] << " " << (int)triple.mBits[1] << " " << (int)triple.mBits[2] << ") = " << triple.val();

        return out;
    }

    std::string prettyShare(int partyIdx, int share0, int share1 = -1)
    {
        std::array<int, 3> shares;
        shares[partyIdx] = share0;
        shares[(partyIdx + 2) % 3] = share1;
        shares[(partyIdx + 1) % 3] = -1;

        std::stringstream ss;
        ss << "(";
        if (shares[0] == -1) ss << "_ ";
        else ss << shares[0] << " ";
        if (shares[1] == -1) ss << "_ ";
        else ss << shares[1] << " ";
        if (shares[2] == -1) ss << "_)";
        else ss << shares[2] << ")";

        return ss.str();
    }

    void Sh3BinaryEvaluator::roundCallback(Sh3::CommPkg& comm, Sh3Task task)
    {

        i32 shareCountDiv8 = (mMem.shareCount() + 7) / 8;
        i32 simdWidth128 = mMem.simdWidth();
        //auto simdWidth1024 = (simdWidth128 + 7) / 8;

        if (mLevel > mCir->mLevelCounts.size())
        {
            throw std::runtime_error("evaluateRound() was called but no rounds remain... " LOCATION);
        }
        if (mCir->mLevelCounts.size() == 0 && mCir->mNonlinearGateCount)
            throw std::runtime_error("the level by and gate function must be called first." LOCATION);

        if (mLevel)
        {
            if (mRecvLocs.size())
            {
                for(auto& fu : mRecvFutr)
                    fu.get();
                mRecvFutr.clear();

                auto iter = mRecvData.begin();

                for (i32 j = 0; j < mRecvLocs.size(); ++j)
                {
                    auto out = mRecvLocs[j];
                    memcpy(&mMem.mShares[1](out), &*iter, shareCountDiv8);
                    iter += shareCountDiv8;

                    //{
                    //    auto tt = iter - shareCountDiv8;
                    //    //for(u64 i = 0; i < )
                    //    ostreamLock o(std::cout);
                    //    o << ((mDebugPartyIdx+2)%3) << " " << out << " - ";
                    //    while (tt != iter)
                    //    {
                    //        o << std::hex << int(*tt++) << " ";
                    //    }
                    //    o << std::dec << std::endl;
                    //}

                    //validateWire(out / simdWidth128);
                }

            }
        }
        else
        {
            mGateIter = mCir->mGates.data();
            auto seed0 = toBlock((task.getRuntime().mPartyIdx));
            auto seed1 = toBlock((task.getRuntime().mPartyIdx + 2) % 3);
            mShareGen.init(seed0, seed1, simdWidth128 * sizeof(block_type) / sizeof(block));
        }

        block_type AllOneBlock;
        memset(&AllOneBlock, 0xFF, sizeof(block_type));

        const bool debug = mDebug;
        auto& shares = mMem.mShares;


        const auto bufferSize = std::max<i32>(1 << 16, shareCountDiv8);

        std::array<block_type, 8> t0, t1, t2;// , t3;


        if (mLevel < mCir->mLevelCounts.size())
        {
            auto andGateCount = mCir->mLevelAndCounts[mLevel];
            auto gateCount = mCir->mLevelCounts[mLevel];

            mRecvLocs.resize(andGateCount);
            auto updateIter = mRecvLocs.data();
            //std::vector<u8> mSendData(andGateCount * shareCountDiv8);
            auto& sendBuff = mSendBuffs[mLevel & 1];
            sendBuff.resize(andGateCount * shareCountDiv8);
            mRecvData.resize(andGateCount * shareCountDiv8);
            auto writeIter = sendBuff.begin();
            auto prevSendIdx = 0;
            auto nextSendIdx = std::min<i32>(bufferSize, sendBuff.size());

            //std::vector<block_type> s0_in0_data(simdWidth128), s0_in1_data(simdWidth128);
            //std::vector<block_type> s1_in0_data(simdWidth128), s1_in1_data(simdWidth128);

            for (i32 j = 0; j < gateCount; ++j, ++mGateIter)
            {
                const auto& gate = *mGateIter;
                const auto& type = gate.mType;
                auto in0 = gate.mInput[0] * simdWidth128;
                auto in1 = gate.mInput[1] * simdWidth128;
                auto out = gate.mOutput * simdWidth128;
                auto s0_Out = &shares[0](out);
                auto s1_Out = &shares[1](out);
                auto s0_in0 = &shares[0](in0);
                auto s0_in1 = &shares[0](in1);
                auto s1_in0 = &shares[1](in0);
                auto s1_in1 = &shares[1](in1);

                std::array<block_type*, 2> z{ nullptr, nullptr };

                //memcpy(s0_in0_data.data(), s0_in0, simdWidth128 * sizeof(block_type));
                //memcpy(s0_in1_data.data(), s0_in1, simdWidth128 * sizeof(block_type));
                //memcpy(s1_in0_data.data(), s1_in0, simdWidth128 * sizeof(block_type));
                //memcpy(s1_in1_data.data(), s1_in1, simdWidth128 * sizeof(block_type));

                switch (gate.mType)
                {
                case GateType::Xor:
                    for (i32 k = 0; k < simdWidth128; k += 8)
                    {
                        s0_Out[k + 0] = s0_in0[k + 0] ^ s0_in1[k + 0];
                        s0_Out[k + 1] = s0_in0[k + 1] ^ s0_in1[k + 1];
                        s0_Out[k + 2] = s0_in0[k + 2] ^ s0_in1[k + 2];
                        s0_Out[k + 3] = s0_in0[k + 3] ^ s0_in1[k + 3];
                        s0_Out[k + 4] = s0_in0[k + 4] ^ s0_in1[k + 4];
                        s0_Out[k + 5] = s0_in0[k + 5] ^ s0_in1[k + 5];
                        s0_Out[k + 6] = s0_in0[k + 6] ^ s0_in1[k + 6];
                        s0_Out[k + 7] = s0_in0[k + 7] ^ s0_in1[k + 7];

                        s1_Out[k + 0] = s1_in0[k + 0] ^ s1_in1[k + 0];
                        s1_Out[k + 1] = s1_in0[k + 1] ^ s1_in1[k + 1];
                        s1_Out[k + 2] = s1_in0[k + 2] ^ s1_in1[k + 2];
                        s1_Out[k + 3] = s1_in0[k + 3] ^ s1_in1[k + 3];
                        s1_Out[k + 4] = s1_in0[k + 4] ^ s1_in1[k + 4];
                        s1_Out[k + 5] = s1_in0[k + 5] ^ s1_in1[k + 5];
                        s1_Out[k + 6] = s1_in0[k + 6] ^ s1_in1[k + 6];
                        s1_Out[k + 7] = s1_in0[k + 7] ^ s1_in1[k + 7];
                    }
                    break;
                case GateType::And:
                    *updateIter++ = out;
                    z = getShares();
                    for (i32 k = 0; k < simdWidth128; k += 8)
                    {
                        t0[0] = s0_in0[k + 0] & s0_in1[k + 0];
                        t0[1] = s0_in0[k + 1] & s0_in1[k + 1];
                        t0[2] = s0_in0[k + 2] & s0_in1[k + 2];
                        t0[3] = s0_in0[k + 3] & s0_in1[k + 3];
                        t0[4] = s0_in0[k + 4] & s0_in1[k + 4];
                        t0[5] = s0_in0[k + 5] & s0_in1[k + 5];
                        t0[6] = s0_in0[k + 6] & s0_in1[k + 6];
                        t0[7] = s0_in0[k + 7] & s0_in1[k + 7];


                        t1[0] = s0_in0[k + 0] & s1_in1[k + 0];
                        t1[1] = s0_in0[k + 1] & s1_in1[k + 1];
                        t1[2] = s0_in0[k + 2] & s1_in1[k + 2];
                        t1[3] = s0_in0[k + 3] & s1_in1[k + 3];
                        t1[4] = s0_in0[k + 4] & s1_in1[k + 4];
                        t1[5] = s0_in0[k + 5] & s1_in1[k + 5];
                        t1[6] = s0_in0[k + 6] & s1_in1[k + 6];
                        t1[7] = s0_in0[k + 7] & s1_in1[k + 7];

                        t0[0] = t0[0] ^ t1[0];
                        t0[1] = t0[1] ^ t1[1];
                        t0[2] = t0[2] ^ t1[2];
                        t0[3] = t0[3] ^ t1[3];
                        t0[4] = t0[4] ^ t1[4];
                        t0[5] = t0[5] ^ t1[5];
                        t0[6] = t0[6] ^ t1[6];
                        t0[7] = t0[7] ^ t1[7];

                        t1[0] = s1_in0[k + 0] & s0_in1[k + 0];
                        t1[1] = s1_in0[k + 1] & s0_in1[k + 1];
                        t1[2] = s1_in0[k + 2] & s0_in1[k + 2];
                        t1[3] = s1_in0[k + 3] & s0_in1[k + 3];
                        t1[4] = s1_in0[k + 4] & s0_in1[k + 4];
                        t1[5] = s1_in0[k + 5] & s0_in1[k + 5];
                        t1[6] = s1_in0[k + 6] & s0_in1[k + 6];
                        t1[7] = s1_in0[k + 7] & s0_in1[k + 7];

                        t0[0] = t0[0] ^ t1[0];
                        t0[1] = t0[1] ^ t1[1];
                        t0[2] = t0[2] ^ t1[2];
                        t0[3] = t0[3] ^ t1[3];
                        t0[4] = t0[4] ^ t1[4];
                        t0[5] = t0[5] ^ t1[5];
                        t0[6] = t0[6] ^ t1[6];
                        t0[7] = t0[7] ^ t1[7];

                        t0[0] = t0[0] ^ z[0][k + 0];
                        t0[1] = t0[1] ^ z[0][k + 1];
                        t0[2] = t0[2] ^ z[0][k + 2];
                        t0[3] = t0[3] ^ z[0][k + 3];
                        t0[4] = t0[4] ^ z[0][k + 4];
                        t0[5] = t0[5] ^ z[0][k + 5];
                        t0[6] = t0[6] ^ z[0][k + 6];
                        t0[7] = t0[7] ^ z[0][k + 7];

                        s0_Out[0] = t0[0] ^ z[1][k + 0];
                        s0_Out[1] = t0[1] ^ z[1][k + 1];
                        s0_Out[2] = t0[2] ^ z[1][k + 2];
                        s0_Out[3] = t0[3] ^ z[1][k + 3];
                        s0_Out[4] = t0[4] ^ z[1][k + 4];
                        s0_Out[5] = t0[5] ^ z[1][k + 5];
                        s0_Out[6] = t0[6] ^ z[1][k + 6];
                        s0_Out[7] = t0[7] ^ z[1][k + 7];

                        //s0_Out[k]
                        //    = (s0_in0[k] & s0_in1[k]) // t0
                        //    ^ (s0_in0[k] & s1_in1[k]) // t1
                        //    ^ (s1_in0[k] & s0_in1[k])
                        //    ^ z[0][k]
                        //    ^ z[1][k];

#ifndef NDEBUG
                        if (eq(s1_in0[k], CCBlock) || eq(s1_in1[k], CCBlock))
                            throw std::runtime_error(LOCATION);
                        s1_Out[k] = CCBlock;
#endif

                    }
                    memcpy(&*writeIter, s0_Out, shareCountDiv8);
                    writeIter += shareCountDiv8;

                    break;
                case GateType::Nor:
                    *updateIter++ = out;
                    z = getShares();
                    for (i32 k = 0; k < simdWidth128; k += 8)
                    {
                        // t0 = mem00
                        t0[0] = s0_in0[k + 0] ^ AllOneBlock;
                        t0[1] = s0_in0[k + 1] ^ AllOneBlock;
                        t0[2] = s0_in0[k + 2] ^ AllOneBlock;
                        t0[3] = s0_in0[k + 3] ^ AllOneBlock;
                        t0[4] = s0_in0[k + 4] ^ AllOneBlock;
                        t0[5] = s0_in0[k + 5] ^ AllOneBlock;
                        t0[6] = s0_in0[k + 6] ^ AllOneBlock;
                        t0[7] = s0_in0[k + 7] ^ AllOneBlock;

                        // t1 = mem01
                        t1[0] = s0_in1[k + 0] ^ AllOneBlock;
                        t1[1] = s0_in1[k + 1] ^ AllOneBlock;
                        t1[2] = s0_in1[k + 2] ^ AllOneBlock;
                        t1[3] = s0_in1[k + 3] ^ AllOneBlock;
                        t1[4] = s0_in1[k + 4] ^ AllOneBlock;
                        t1[5] = s0_in1[k + 5] ^ AllOneBlock;
                        t1[6] = s0_in1[k + 6] ^ AllOneBlock;
                        t1[7] = s0_in1[k + 7] ^ AllOneBlock;

                        // t2 = mem10
                        t2[0] = s1_in0[k + 0] ^ AllOneBlock;
                        t2[1] = s1_in0[k + 1] ^ AllOneBlock;
                        t2[2] = s1_in0[k + 2] ^ AllOneBlock;
                        t2[3] = s1_in0[k + 3] ^ AllOneBlock;
                        t2[4] = s1_in0[k + 4] ^ AllOneBlock;
                        t2[5] = s1_in0[k + 5] ^ AllOneBlock;
                        t2[6] = s1_in0[k + 6] ^ AllOneBlock;
                        t2[7] = s1_in0[k + 7] ^ AllOneBlock;

                        // out = mem11
                        s0_Out[0] = s1_in1[k + 0] ^ AllOneBlock;
                        s0_Out[1] = s1_in1[k + 1] ^ AllOneBlock;
                        s0_Out[2] = s1_in1[k + 2] ^ AllOneBlock;
                        s0_Out[3] = s1_in1[k + 3] ^ AllOneBlock;
                        s0_Out[4] = s1_in1[k + 4] ^ AllOneBlock;
                        s0_Out[5] = s1_in1[k + 5] ^ AllOneBlock;
                        s0_Out[6] = s1_in1[k + 6] ^ AllOneBlock;
                        s0_Out[7] = s1_in1[k + 7] ^ AllOneBlock;

                        // t3 = mem11 & mem00
                        s0_Out[0] = s0_Out[0] & t0[0];
                        s0_Out[1] = s0_Out[1] & t0[1];
                        s0_Out[2] = s0_Out[2] & t0[2];
                        s0_Out[3] = s0_Out[3] & t0[3];
                        s0_Out[4] = s0_Out[4] & t0[4];
                        s0_Out[5] = s0_Out[5] & t0[5];
                        s0_Out[6] = s0_Out[6] & t0[6];
                        s0_Out[7] = s0_Out[7] & t0[7];

                        // t2 = mem10 & mem01
                        t2[0] = t2[0] & t1[0];
                        t2[1] = t2[1] & t1[1];
                        t2[2] = t2[2] & t1[2];
                        t2[3] = t2[3] & t1[3];
                        t2[4] = t2[4] & t1[4];
                        t2[5] = t2[5] & t1[5];
                        t2[6] = t2[6] & t1[6];
                        t2[7] = t2[7] & t1[7];

                        // out = mem11 & mem00 ^ mem10 & mem01
                        s0_Out[0] = s0_Out[0] ^ t2[0];
                        s0_Out[1] = s0_Out[1] ^ t2[1];
                        s0_Out[2] = s0_Out[2] ^ t2[2];
                        s0_Out[3] = s0_Out[3] ^ t2[3];
                        s0_Out[4] = s0_Out[4] ^ t2[4];
                        s0_Out[5] = s0_Out[5] ^ t2[5];
                        s0_Out[6] = s0_Out[6] ^ t2[6];
                        s0_Out[7] = s0_Out[7] ^ t2[7];

                        // t1 = mem00 & mem01
                        t1[0] = t0[0] & t1[0];
                        t1[1] = t0[1] & t1[1];
                        t1[2] = t0[2] & t1[2];
                        t1[3] = t0[3] & t1[3];
                        t1[4] = t0[4] & t1[4];
                        t1[5] = t0[5] & t1[5];
                        t1[6] = t0[6] & t1[6];
                        t1[7] = t0[7] & t1[7];

                        // out = mem11 & mem00 ^ mem10 & mem01 ^ mem00 & mem01
                        s0_Out[0] = s0_Out[0] ^ t1[0];
                        s0_Out[1] = s0_Out[1] ^ t1[1];
                        s0_Out[2] = s0_Out[2] ^ t1[2];
                        s0_Out[3] = s0_Out[3] ^ t1[3];
                        s0_Out[4] = s0_Out[4] ^ t1[4];
                        s0_Out[5] = s0_Out[5] ^ t1[5];
                        s0_Out[6] = s0_Out[6] ^ t1[6];
                        s0_Out[7] = s0_Out[7] ^ t1[7];

                        // out = mem11 & mem00 ^ mem10 & mem01 ^ mem00 & mem01 ^ z0
                        s0_Out[0] = s0_Out[0] ^ z[0][k + 0];
                        s0_Out[1] = s0_Out[1] ^ z[0][k + 1];
                        s0_Out[2] = s0_Out[2] ^ z[0][k + 2];
                        s0_Out[3] = s0_Out[3] ^ z[0][k + 3];
                        s0_Out[4] = s0_Out[4] ^ z[0][k + 4];
                        s0_Out[5] = s0_Out[5] ^ z[0][k + 5];
                        s0_Out[6] = s0_Out[6] ^ z[0][k + 6];
                        s0_Out[7] = s0_Out[7] ^ z[0][k + 7];

                        // out = mem11 & mem00 ^ mem10 & mem01 ^ mem00 & mem01 ^ z0 ^ z1
                        s0_Out[0] = s0_Out[0] ^ z[1][k + 0];
                        s0_Out[1] = s0_Out[1] ^ z[1][k + 1];
                        s0_Out[2] = s0_Out[2] ^ z[1][k + 2];
                        s0_Out[3] = s0_Out[3] ^ z[1][k + 3];
                        s0_Out[4] = s0_Out[4] ^ z[1][k + 4];
                        s0_Out[5] = s0_Out[5] ^ z[1][k + 5];
                        s0_Out[6] = s0_Out[6] ^ z[1][k + 6];
                        s0_Out[7] = s0_Out[7] ^ z[1][k + 7];

                        //auto mem00 = s0_in0[k] ^ AllOneBlock;
                        //auto mem01 = s0_in1[k] ^ AllOneBlock;
                        //auto mem10 = s1_in0[k] ^ AllOneBlock;
                        //auto mem11 = s1_in1[k] ^ AllOneBlock;

                        //s0_Out[k]
                        //    = (mem00 & mem01) // t1
                        //    ^ (mem00 & mem11) // t3
                        //    ^ (mem10 & mem01) // t2
                        //    ^ z[0][k]
                        //    ^ z[1][k];

#ifndef NDEBUG
                        if (eq(s1_in0[k], CCBlock) || eq(s1_in1[k], CCBlock))
                            throw std::runtime_error(LOCATION);
                        s1_Out[k] = CCBlock;
#endif
                    }

                    memcpy(&*writeIter, s0_Out, shareCountDiv8);
                    writeIter += shareCountDiv8;
                    break;
                case GateType::Nxor:
                    for (i32 k = 0; k < simdWidth128; k += 8)
                    {
                        s0_Out[k + 0] = s0_in0[k + 0] ^ s0_in1[k + 0];
                        s0_Out[k + 1] = s0_in0[k + 1] ^ s0_in1[k + 1];
                        s0_Out[k + 2] = s0_in0[k + 2] ^ s0_in1[k + 2];
                        s0_Out[k + 3] = s0_in0[k + 3] ^ s0_in1[k + 3];
                        s0_Out[k + 4] = s0_in0[k + 4] ^ s0_in1[k + 4];
                        s0_Out[k + 5] = s0_in0[k + 5] ^ s0_in1[k + 5];
                        s0_Out[k + 6] = s0_in0[k + 6] ^ s0_in1[k + 6];
                        s0_Out[k + 7] = s0_in0[k + 7] ^ s0_in1[k + 7];

                        s1_Out[k + 0] = s1_in0[k + 0] ^ s1_in1[k + 0];
                        s1_Out[k + 1] = s1_in0[k + 1] ^ s1_in1[k + 1];
                        s1_Out[k + 2] = s1_in0[k + 2] ^ s1_in1[k + 2];
                        s1_Out[k + 3] = s1_in0[k + 3] ^ s1_in1[k + 3];
                        s1_Out[k + 4] = s1_in0[k + 4] ^ s1_in1[k + 4];
                        s1_Out[k + 5] = s1_in0[k + 5] ^ s1_in1[k + 5];
                        s1_Out[k + 6] = s1_in0[k + 6] ^ s1_in1[k + 6];
                        s1_Out[k + 7] = s1_in0[k + 7] ^ s1_in1[k + 7];

                        s0_Out[k + 0] = s0_Out[k + 0] ^ AllOneBlock;
                        s0_Out[k + 1] = s0_Out[k + 1] ^ AllOneBlock;
                        s0_Out[k + 2] = s0_Out[k + 2] ^ AllOneBlock;
                        s0_Out[k + 3] = s0_Out[k + 3] ^ AllOneBlock;
                        s0_Out[k + 4] = s0_Out[k + 4] ^ AllOneBlock;
                        s0_Out[k + 5] = s0_Out[k + 5] ^ AllOneBlock;
                        s0_Out[k + 6] = s0_Out[k + 6] ^ AllOneBlock;
                        s0_Out[k + 7] = s0_Out[k + 7] ^ AllOneBlock;

                        s1_Out[k + 0] = s1_Out[k + 0] ^ AllOneBlock;
                        s1_Out[k + 1] = s1_Out[k + 1] ^ AllOneBlock;
                        s1_Out[k + 2] = s1_Out[k + 2] ^ AllOneBlock;
                        s1_Out[k + 3] = s1_Out[k + 3] ^ AllOneBlock;
                        s1_Out[k + 4] = s1_Out[k + 4] ^ AllOneBlock;
                        s1_Out[k + 5] = s1_Out[k + 5] ^ AllOneBlock;
                        s1_Out[k + 6] = s1_Out[k + 6] ^ AllOneBlock;
                        s1_Out[k + 7] = s1_Out[k + 7] ^ AllOneBlock;

                    }
                    break;
                case GateType::a:
                    for (i32 k = 0; k < simdWidth128; ++k)
                    {
#ifndef NDEBUG
                        if (eq(s1_in0[k], CCBlock))
                            throw std::runtime_error(LOCATION);
#endif
                        TODO("vectorize");
                        s0_Out[k] = s0_in0[k];
                        s1_Out[k] = s1_in0[k];

                    }
                    break;
                case GateType::na_And:
                    z = getShares();

                    *updateIter++ = out;
                    for (i32 k = 0; k < simdWidth128; ++k)
                    {
                        TODO("vectorize");
                        s0_Out[k]
                            = ((AllOneBlock ^ s0_in0[k]) & s0_in1[k])
                            ^ ((AllOneBlock ^ s0_in0[k]) & s1_in1[k])
                            ^ ((AllOneBlock ^ s1_in0[k]) & s0_in1[k])
                            ^ z[0][k]
                            ^ z[1][k];

#ifndef NDEBUG
                        if (eq(s1_in0[k], CCBlock) || eq(s1_in1[k], CCBlock))
                            throw std::runtime_error(LOCATION);
                        s1_Out[k] = CCBlock;
#endif
                    }

                    memcpy(&*writeIter, s0_Out, shareCountDiv8);
                    writeIter += shareCountDiv8;
                    break;
                case GateType::Zero:
                case GateType::nb_And:
                case GateType::nb:
                case GateType::na:
                case GateType::Nand:
                case GateType::nb_Or:
                case GateType::b:
                case GateType::na_Or:
                case GateType::Or:
                case GateType::One:
                default:

                    throw std::runtime_error("BinaryEngine unsupported GateType " LOCATION);
                    break;
                }

                if (false)
                {
                    auto size = nextSendIdx - prevSendIdx;
                    if (size && writeIter - sendBuff.begin() >= nextSendIdx)
                    {
                        comm.mNext.asyncSend(&*sendBuff.begin() + prevSendIdx, size);
                        mRecvFutr.emplace_back(comm.mPrev.asyncRecv(&*mRecvData.begin() + prevSendIdx, size));

                        prevSendIdx = nextSendIdx;
                        nextSendIdx = std::min<i32>(nextSendIdx + bufferSize, sendBuff.size());
                    }
                }


#ifdef BINARY_ENGINE_DEBUG
                if (debug)
                {
                    auto gIdx = mGateIter - mCir->mGates.data();
                    auto gIn0 = gate.mInput[0];
                    auto gIn1 = gate.mInput[1];
                    auto gOut = gate.mOutput;

                    //if (gIdx % 1000 == 0)
                    //{
                    //    block b = hashDebugState();
                    //    ostreamLock(std::cout) << "b" << mDebugPartyIdx << " g" << gIdx << " " << b << std::endl;
                    //}
                    //if (gate.mType == GateType::And)
                    //{
                    //    auto iter = writeIter - shareCountDiv8;
                    //    //for(u64 i = 0; i < )
                    //    ostreamLock o(std::cout);
                    //    o << mDebugPartyIdx<< " " << gOut << " - ";
                    //    while (iter != writeIter)
                    //    {
                    //        o << std::hex << int(*iter++) << " ";
                    //    }
                    //    o << std::dec << std::endl;
                    //}

                    auto prevIdx = (mDebugPartyIdx + 2) % 3;
                    for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
                    {
                        auto s0_out = extractBitShare(r, gOut, 0);
                        auto s0_in0_val = extractBit((i64*)s0_in0, r);
                        auto s1_in0_val = extractBit((i64*)s1_in0, r);
                        auto s0_in1_val = extractBit((i64*)s0_in1, r);
                        auto s1_in1_val = extractBit((i64*)s1_in1, r);

                        auto& m = mPlainWires_DEBUG[r];
                        auto inTriple0 = m[gIn0];
                        auto inTriple1 = m[gIn1];

                        m[gOut].assign(inTriple0, inTriple1, gate.mType);

                        auto badOutput = s0_out != m[gOut].mBits[mDebugPartyIdx];
                        auto badInput0 = 
                            s0_in0_val != inTriple0.mBits[mDebugPartyIdx] ||
                            s1_in0_val != inTriple0.mBits[prevIdx];
                        auto badInput1 = 
                            s0_in1_val != inTriple1.mBits[mDebugPartyIdx] ||
                            s1_in1_val != inTriple1.mBits[prevIdx];

                        if (gIn0 == gOut){
                            badInput0 = false;
                            s0_in0_val = -1;
                            s1_in0_val = -1;
                        }

                        if (gIn1 == gOut) {
                            badInput1 = false;
                            s0_in1_val = -1;
                            s1_in1_val = -1;
                        }

                        auto bad = badOutput || badInput0 || badInput1;

                        if (bad)
                        {


                            ostreamLock(std::cout)
                                << "\np "<< mDebugPartyIdx << " gate[" << gIdx << "] r" << r <<": "
                                << gIn0 << " " << gateToString(type) << " " << gIn1 << " -> " << int(gOut)
                                << " exp: " << m[gOut] << "\n"
                                << " act: " << prettyShare(mDebugPartyIdx, s0_out) << "\n"
                                << " in0: " << inTriple0 << "\n"
                                << "*in0: " << prettyShare(mDebugPartyIdx, s0_in0_val, s1_in0_val) << "\n"
                                << " in1: " << inTriple1 << "\n"
                                << "*in1: " << prettyShare(mDebugPartyIdx, s0_in1_val, s1_in1_val) << "\n"
                                << std::endl;

                            if (bad)
                            {

                                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                                throw std::runtime_error(LOCATION);
                            }
                        }
                    }

                }
#endif
            }

            if (writeIter != sendBuff.end())
                throw std::runtime_error(LOCATION);

            if (false)
            {
                if (prevSendIdx != sendBuff.size())
                    throw std::runtime_error(LOCATION);
            }
            else
            {
                if (sendBuff.size())
                {
                    comm.mNext.asyncSend(sendBuff.data(), sendBuff.size());
                    mRecvData.resize(sendBuff.size());
                    mRecvFutr.emplace_back(comm.mPrev.asyncRecv(mRecvData));
                }
            }

            //SHA1 sha(sizeof(block));
            //sha.Update(sendBuff.data(), sendBuff.size());
            //block b;
            //sha.Final(b);
            //ostreamLock(std::cout) << b << std::endl;

            //


        }

        mLevel++;
        if (mGateIter > mCir->mGates.data() + mCir->mGates.size()) throw std::runtime_error(LOCATION);


        if (hasMoreRounds())
        {
            task.nextRound([this](Sh3::CommPkg& comm, Sh3Task& task)
            {
                roundCallback(comm, task);
            }
            );
        }
    }

    void Sh3BinaryEvaluator::getOutput(u64 i, Sh3::sbMatrix & out)
    {
        if (mCir->mOutputs.size() <= i) throw std::runtime_error(LOCATION);

        const auto& outWires = mCir->mOutputs[i].mWires;

        getOutput(outWires, out);
    }

    void Sh3BinaryEvaluator::getOutput(u64 idx, Sh3::sPackedBin & out)
    {
        const auto& outWires = mCir->mOutputs[idx].mWires;
        getOutput(outWires, out);
    }

    void Sh3BinaryEvaluator::getOutput(const std::vector<oc::BetaWire>& outWires, Sh3::sPackedBin & out)
    {

        out.resize(mMem.shareCount(), outWires.size());

        auto simdWidth128 = mMem.simdWidth();

        for (u64 j = 0; j < 2; ++j)
        {
            auto prevIdx = (mDebugPartyIdx + 2) % 3;
            auto jj = !j ? mDebugPartyIdx : prevIdx;
            auto dest = out.mShares[j].data();

            for (u64 wireIdx = 0; wireIdx < outWires.size(); ++wireIdx)
            {
                //if (mCir->isInvert(outWires[wireIdx]))
                //	throw std::runtime_error(LOCATION);
                auto wire = outWires[wireIdx];

                auto md = mMem.mShares[j].data();
                auto ms = mMem.mShares[j].size();
                auto src = md + wire * simdWidth128;
                auto size = out.simdWidth() * sizeof(i64);

                //if (src + simdWidth > md + ms)
                //    throw std::runtime_error(LOCATION);
                //if (dest + simdWidth > out..data() + outMem.size())
                //    throw std::runtime_error(LOCATION);


                memcpy(dest, src, size);

                // check if we need to ivert these output wires.
                if (mCir->isInvert(wire))
                {
                    for (u64 k = 0; k < out.simdWidth(); ++k)
                    {
                        dest[k] = ~dest[k];
                    }
                }

                dest += out.simdWidth();

#ifdef BINARY_ENGINE_DEBUG
                for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
                {
                    auto m = mPlainWires_DEBUG[r];
                    //auto k = r / (sizeof(i64) * 8);
                    //auto l = r % (sizeof(i64) * 8);
                    //i64 plain = (((u64*)src[k] >> l) & 1;
                    auto bit0 = extractBitShare(r, wire, j);

                    if (m[wire].mBits[jj] != bit0)
                        throw std::runtime_error(LOCATION);
                }
#endif
            }
        }
    }

    void Sh3BinaryEvaluator::getOutput(const std::vector<BetaWire>& outWires, Sh3::sbMatrix & out)
    {

        using Word = i64;
        if (outWires.size() != out.bitCount()) throw std::runtime_error(LOCATION);
        //auto outCols = roundUpTo(outWires.size(), 8);

        block_type AllOneBlock;
        memset(&AllOneBlock, 0xFF, sizeof(block_type));
        auto simdWidth128 = mMem.simdWidth();
        //auto simdWidth64 = (mMem.shareCount() + 63) / 64;
        Eigen::Matrix<block_type, Eigen::Dynamic, Eigen::Dynamic> temp;
        temp.resize(outWires.size(), simdWidth128);

        for (u64 j = 0; j < 2; ++j)
        {
            auto dest = temp.data();

            for (u64 i = 0; i < outWires.size(); ++i)
            {
                //if (mCir->isInvert(outWires[i]))
                //	throw std::runtime_error(LOCATION);

                auto md = mMem.mShares[j].data();
                auto ms = mMem.mShares[j].size();
                auto src = md + outWires[i] * simdWidth128;
                auto size = simdWidth128 * sizeof(block_type);

                if (src + simdWidth128 > md + ms)
                    throw std::runtime_error(LOCATION);
                if (dest + simdWidth128 > temp.data() + temp.size())
                    throw std::runtime_error(LOCATION);


                memcpy(dest, src, size);

                // check if we need to ivert these output wires.
                if (mCir->isInvert(outWires[i]))
                {
                    for (u64 k = 0; k < simdWidth128; ++k)
                    {
                        dest[k] = dest[k] ^ AllOneBlock;
                    }
                }

                dest += simdWidth128;

#ifdef BINARY_ENGINE_DEBUG
                if (mDebug)
                {
                    auto prev = (mDebugPartyIdx + 2) % 3;
                    auto jj = !j ? mDebugPartyIdx : prev;
                    for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
                    {
                        auto m = mPlainWires_DEBUG[r];
                        //auto k = r / (sizeof(Word) * 8);
                        //auto l = r % (sizeof(Word) * 8);
                        //Word plain = (src[k] >> l) & 1;

                        auto bit0 = extractBitShare(r, outWires[i], j);
                        //if (j == 1 && mDebugPartyIdx == 1 && i == 0 && r == 0)
                        //{
                        //    ostreamLock o(std::cout);
                        //    o << "hashState " << hashState() << std::endl;

                        //    for (u64 hh = 0; hh < simdWidth; ++hh)
                        //    {
                        //        o << " SS[" << hh << "] " << src[hh] << std::endl;
                        //    }

                        //    o << "ss " << int(m[outWires[i]].mBits[jj]) << " != " << plain << " = (" << src[k] <<" >> " << l<< ") & 1  : " << k << std::endl;
                        //}
                        if (m[outWires[i]].mBits[jj] != bit0)
                            throw std::runtime_error(LOCATION);
                    }
                }
#endif
            }
            MatrixView<u8> in((u8*)temp.data(), (u8*)(temp.data() + temp.size()), simdWidth128 * sizeof(block_type));
            MatrixView<u8> oout((u8*)out.mShares[j].data(), (u8*)(out.mShares[j].data() + out.mShares[j].size()), sizeof(Word) * out.mShares[j].cols());
            //memset(oout.data(), 0, oout.size());
            //out.mShares[j].setZero();
            memset(out.mShares[j].data(), 0, out.mShares[j].size() * sizeof(i64));
            sse_transpose(in, oout);

#ifdef BINARY_ENGINE_DEBUG
            if (mDebug)
            {

                auto prev = (mDebugPartyIdx + 2) % 3;
                auto jj = !j ? mDebugPartyIdx : prev;

                for (u64 i = 0; i < outWires.size(); ++i)
                {
                    for (u64 r = 0; r < mPlainWires_DEBUG.size(); ++r)
                    {
                        auto m = mPlainWires_DEBUG[r];
                        auto k = i / (sizeof(Word) * 8);
                        auto l = i % (sizeof(Word) * 8);
                        auto outWord = out.mShares[j](r, k);
                        auto plain = (outWord >> l) & 1;

                        auto inv = mCir->isInvert(outWires[i]) & 1;
                        auto xx = m[outWires[i]].mBits[jj] ^ inv;
                        if (xx != plain)
                            throw std::runtime_error(LOCATION);
                    }
                }
                auto mod = (outWires.size() % 64);
                u64 mask = ~(mod ? (Word(1) << mod) - 1 : -1);
                for (u64 i = 0; i < out.rows(); ++i)
                {
                    auto cols = out.mShares[j].cols();
                    if (out.mShares[j][i][cols - 1] & mask)
                        throw std::runtime_error(LOCATION);
                }
            }
#endif
        }
    }

    std::array<Sh3BinaryEvaluator::block_type*, 2> Sh3BinaryEvaluator::getShares()
    {

        mShareGen.refillBuffer();
        auto* z0 = (block_type*)mShareGen.mShareBuff[0].data();
        auto* z1 = (block_type*)mShareGen.mShareBuff[1].data();

#ifdef BINARY_ENGINE_DEBUG
        if (mDebug)
        {
            memset(mShareGen.mShareBuff[0].data(), 0, mShareGen.mShareBuff[0].size() * sizeof(block));
            memset(mShareGen.mShareBuff[1].data(), 0, mShareGen.mShareBuff[0].size() * sizeof(block));
        }
#endif
        return { z0, z1 };
    }


#ifdef BINARY_ENGINE_DEBUG

    void Sh3BinaryEvaluator::DEBUG_Triple::assign(
        const DEBUG_Triple & in0,
        const DEBUG_Triple & in1,
        GateType type)
    {
        mIsSet = true;
        auto vIn0 = int(in0.val());
        auto vIn1 = int(in1.val());

        if (vIn0 > 1) throw std::runtime_error(LOCATION);
        if (vIn1 > 1) throw std::runtime_error(LOCATION);

        u8 plain;
        if (type == GateType::Xor)
        {
            plain = vIn0 ^ vIn1;
            std::array<u8, 3> t;

            t[0] = in0.mBits[0] ^ in1.mBits[0];
            t[1] = in0.mBits[1] ^ in1.mBits[1];
            t[2] = in0.mBits[2] ^ in1.mBits[2];

            mBits = t;
        }
        else if (type == GateType::Nxor)
        {
            plain = vIn0 ^ vIn1 ^ 1;

            std::array<u8, 3> t;

            t[0] = in0.mBits[0] ^ in1.mBits[0] ^ 1;
            t[1] = in0.mBits[1] ^ in1.mBits[1] ^ 1;
            t[2] = in0.mBits[2] ^ in1.mBits[2] ^ 1;

            mBits = t;
        }
        else if (type == GateType::And)
        {
            plain = vIn0 & vIn1;
            std::array<u8, 3> t;
            for (u64 b = 0; b < 3; ++b)
            {

                auto bb = b ? (b - 1) : 2;

                if (bb != (b + 2) % 3) throw std::runtime_error(LOCATION);

                auto in00 = in0.mBits[b];
                auto in01 = in0.mBits[bb];
                auto in10 = in1.mBits[b];
                auto in11 = in1.mBits[bb];

                t[b]
                    = in00 & in10
                    ^ in00 & in11
                    ^ in01 & in10;
            }

            mBits = t;
        }

        else if (type == GateType::na_And)
        {
            plain = (1 ^ vIn0) & vIn1;
            std::array<u8, 3> t;
            for (u64 b = 0; b < 3; ++b)
            {

                auto bb = b ? (b - 1) : 2;

                if (bb != (b + 2) % 3) throw std::runtime_error(LOCATION);

                auto in00 = 1 ^ in0.mBits[b];
                auto in01 = 1 ^ in0.mBits[bb];
                auto in10 = in1.mBits[b];
                auto in11 = in1.mBits[bb];

                t[b]
                    = in00 & in10
                    ^ in00 & in11
                    ^ in01 & in10;
            }

            mBits = t;
        }
        else if (type == GateType::Nor)
        {
            plain = (vIn0 | vIn1) ^ 1;


            std::array<u8, 3> t;
            for (u64 b = 0; b < 3; ++b)
            {
                auto bb = b ? (b - 1) : 2;
                auto in00 = 1 ^ in0.mBits[b];
                auto in01 = 1 ^ in0.mBits[bb];
                auto in10 = 1 ^ in1.mBits[b];
                auto in11 = 1 ^ in1.mBits[bb];

                t[b]
                    = in00 & in10
                    ^ in00 & in11
                    ^ in01 & in10;
            }

            mBits = t;
        }
        else if (type == GateType::a)
        {
            plain = vIn0;
            mBits = in0.mBits;
        }
        else
            throw std::runtime_error(LOCATION);

        if (plain != (mBits[0] ^ mBits[1] ^ mBits[2]))
        {
            //ostreamLock(std::cout) /*<< " g" << (mGateIter - mCir->mGates.data())
            //<< " act: " << plain << "   exp: " << (int)m[gate.mOutput] << std::endl*/

            //	<< vIn0 << " " << gateToString(type) << " " << vIn1 << " -> " << int(m[gOut]) << std::endl
            //	<< gIn0 << " " << gateToString(type) << " " << gIn1 << " -> " << int(gOut) << std::endl;
            //std::this_thread::sleep_for(std::chrono::milliseconds(100));
            throw std::runtime_error(LOCATION);
        }
    }
#endif
}
