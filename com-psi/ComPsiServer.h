#pragma once

#include <cryptoTools/Network/Channel.h>
#include <cryptoTools/Crypto/PRNG.h>
#include <cryptoTools/Common/MatrixView.h>
#include "aby3/Engines/sh3/Sh3BinaryEvaluator.h"
#include "aby3/Engines/sh3/Sh3Encryptor.h"
#include "aby3/Engines/sh3/Sh3Evaluator.h"
#include "LowMC.h"
#include <cryptoTools/Common/CuckooIndex.h>
#include <cryptoTools/Common/Timer.h>

#include "com-psi/Table.h"

namespace osuCrypto
{
    using CommPkg = aby3::Sh3::CommPkg;


    class ComPsiServer :public TimerAdapter
    {
    public:
        u64 mIdx, mKeyBitCount = 80;
        CommPkg mComm;
        PRNG mPrng;

        aby3::Sh3Runtime mRt;
        aby3::Sh3Encryptor mEnc;

        void init(u64 idx, Session& prev, Session& next);


        SharedTable localInput(Table& t);
        SharedTable remoteInput(u64 partyIdx);


        SharedTable intersect(SharedTable& A, SharedTable& B);


        // join on leftJoinCol == rightJoinCol and select the select values.
        SharedTable join(
            SharedTable::ColRef leftJoinCol,
            SharedTable::ColRef rightJoinCol,
            std::vector<SharedTable::ColRef> selects
        );



        Matrix<u8> cuckooHashRecv(SharedTable & A);
        void cuckooHashSend(SharedTable & A, CuckooParam& cuckooParams);
        Matrix<u8> cuckooHash(span<SharedTable::ColRef> selects, aby3::Sh3::i64Matrix& keys);


        void selectCuckooPos(MatrixView<u8> cuckooHashTable, std::array<MatrixView<u8>, 3> dest);
        void selectCuckooPos(u32 destRows, u32 srcRows, u32 bytes);
        void selectCuckooPos(MatrixView<u8> cuckooHashTable, std::array<MatrixView<u8>, 3> dest, aby3::Sh3::i64Matrix& keys);


        void compare(SharedTable& B, std::array<MatrixView<u8>,3> selectedA, aby3::Sh3::sPackedBin& intersectionFlags);
        void compare(SharedTable& B, aby3::Sh3::sPackedBin& intersectionFlags);

        aby3::Sh3::i64Matrix computeKeys(span<SharedTable::ColRef> tables, span<u64> reveals);


        BetaCircuit getBasicCompare();
        //LowMC2<> mLowMC;
        BetaCircuit mLowMCCir;




        void p0CheckSelect(MatrixView<u8> cuckoo, std::array<MatrixView<u8>, 3> a2);
        void p1CheckSelect(Matrix<u8> cuckoo, std::array<MatrixView<u8>, 3> a2, aby3::Sh3::i64Matrix& keys);
    };

}