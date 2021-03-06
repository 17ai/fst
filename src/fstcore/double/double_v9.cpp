/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/

// Framework libraries
#include <blockstreamer/blockstreamer_v2.h>
#include <compression/compressor.h>
#include <interface/fstdefines.h>


using namespace std;

void fdsWriteRealVec_v9(ofstream &myfile, double* doubleVector, unsigned long long nrOfRows, unsigned int compression, std::string annotation)
{
  int blockSize = 8 * BLOCKSIZE_REAL;  // block size in bytes

  if (compression == 0)
  {
    return fdsStreamUncompressed_v2(myfile, reinterpret_cast<char*>(doubleVector), nrOfRows, 8, BLOCKSIZE_REAL, nullptr, annotation);
  }

  if (compression <= 50)  // low compression: linear mix of uncompressed LZ4
  {
    Compressor* compress1 = new SingleCompressor(CompAlgo::LZ4, 2 * compression);
    StreamCompressor* streamCompressor = new StreamLinearCompressor(compress1, 2 * compression);
    streamCompressor->CompressBufferSize(blockSize);
    fdsStreamcompressed_v2(myfile, reinterpret_cast<char*>(doubleVector), nrOfRows, 8, streamCompressor, BLOCKSIZE_REAL, annotation);

    delete compress1;
    delete streamCompressor;
    return;
  }

  Compressor* compress1 = new SingleCompressor(CompAlgo::LZ4, 100);
  Compressor* compress2 = new SingleCompressor(CompAlgo::ZSTD, 20);
  StreamCompressor* streamCompressor = new StreamCompositeCompressor(compress1, compress2, 2 * (compression - 50));
  streamCompressor->CompressBufferSize(blockSize);
  fdsStreamcompressed_v2(myfile, reinterpret_cast<char*>(doubleVector), nrOfRows, 8, streamCompressor, BLOCKSIZE_REAL, annotation);

  delete compress1;
  delete compress2;
  delete streamCompressor;

  return;
}


void fdsReadRealVec_v9(istream &myfile, double* doubleVector, unsigned long long blockPos, unsigned long long startRow,
  unsigned long long length, unsigned long long size, std::string &annotation)
{
  return fdsReadColumn_v2(myfile, reinterpret_cast<char*>(doubleVector), blockPos, startRow, length, size, 8, annotation, BATCH_SIZE_READ_DOUBLE);
}
