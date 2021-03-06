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


#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

#include <interface/istringwriter.h>
#include <interface/ifsttable.h>
#include <interface/icolumnfactory.h>
#include <interface/fstdefines.h>
#include <interface/fststore.h>

#include <character/character_v6.h>
#include <factor/factor_v7.h>
#include <integer/integer_v8.h>
#include <double/double_v9.h>
#include <logical/logical_v10.h>
#include <integer64/integer64_v11.h>
#include <byte/byte_v12.h>

#include <ZSTD/common/xxhash.h>

using namespace std;


// Table header [node A] [size: 44]
//
//  8                      | unsigned long long | hash value         // hash of table header
//  4                      | unsigned int       | FST_VERSION        // table header fstcore version
//  4                      | int                | table flags        // binary table flags
//  8                      |                    | free bytes         // possible future use
//  4                      | unsigned int       | FST_VERSION_MAX    // minimum fstcore version required
//  4                      | int                | nrOfCols           // total number of columns in primary chunkset
//  8                      | unsigned long long | primaryChunkSetLoc // reference to the table's primary chunkset
//  4                      | int                | keyLength          // number of keys in table

// Key index vector (only needed when keyLength > 0) [attached leaf of A] [size: 8 + 4 * keyLength]
//
//  8                      | unsigned long long | hash value         // hash of key index vector (if present)
//  4 * keyLength          | int                | keyColPos          // key column indexes in the first horizontal chunk

// Chunkset header [node C, free leaf of A or other chunkset header] [size: 76 + 8 * nrOfCols]
//
//  8                      | unsigned long long | hash value         // hash of chunkset header
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | chunkset flags     // binary horizontal chunk flags
//  8                      |                    | free bytes         // possible future use
//  8                      |                    | free bytes         // possible future use
//  8                      | unsigned long long | colNamesPos        // reference to column names vector
//  8                      | unsigned long long | nextHorzChunkSet   // reference to next chunkset header (additional columns)
//  8                      | unsigned long long | primChunksetIndex  // reference to primary chunkset data (nrOfCols columns)
//  8                      | unsigned long long | secChunksetIndex   // reference to primary chunkset data (nrOfCols columns)
//  8                      | unsigned long long | nrOfRows           // total number of rows in chunkset
//  4                      | int                | p_nrOfChunksetCols // number of columns in primary chunkset
//  2 * nrOfCols           | unsigned short int | colAttributesType  // column attributes
//  2 * nrOfCols           | unsigned short int | colTypes           // column types
//  2 * nrOfCols           | unsigned short int | colBaseTypes       // column base types
//  2 * nrOfCols           | unsigned short int | colScales          // column scales (pico, nano, micro, milli, kilo, mega, giga, tera etc.)

// Column names [leaf to C]  [size: 24 + x]
//
//  8                      | unsigned long long | hash value         // hash of column names header
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | colNames flags     // binary horizontal chunk flags
//  8                      |                    | free bytes         // possible future use
//  x                      | char               | colNames           // column names (internally hashed)

// Chunk index [node D, leaf of C] [size: 96]
//
//  8                      | unsigned long long | hash value         // hash of chunkset data header
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | index flags        // binary horizontal chunk flags
//  8                      |                    | free bytes         // possible future use
//  2                      | unsigned int       | nrOfChunkSlots     // number of chunk slots
//  6                      |                    | free bytes         // possible future use
//  8 * 4                  | unsigned long long | chunkPos           // data chunk addresses
//  8 * 4                  | unsigned long long | chunkRows          // data chunk number of rows

// Data chunk header [node E, leaf of D] [size: 24 + 8 * nrOfCols]
//
//  8                      | unsigned long long | hash value         // hash of chunkset data header
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | data chunk flags
//  8                      |                    | free bytes         // possible future use
//  8 * nrOfCols           | unsigned long long | positionData       // columnar position data
//

// Column data blocks [leaf of E]
//  y                      |                    | column data        // data blocks with column element values


FstStore::FstStore(std::string fstFile)
{
  this->fstFile       = fstFile;
  this->blockReader   = nullptr;
  this->keyColPos     = nullptr;
  this->p_nrOfRows    = nullptr;
  this->metaDataBlock = nullptr;
}


/**
 * \brief Read header information from a fst file
 * \param myfile a stream to a fst file
 * \param keyLength the number of key columns (output)
 * \param nrOfColsFirstChunk the number of columns in the first chunkset (output)
 * \return 
 */
inline unsigned int ReadHeader(ifstream &myfile, int &keyLength, int &nrOfColsFirstChunk)
{
  // Get meta-information for table
  char tableMeta[TABLE_META_SIZE];
  myfile.read(tableMeta, TABLE_META_SIZE);

  if (!myfile)
  {
    myfile.close();
    throw(runtime_error(FSTERROR_ERROR_OPEN_READ));
  }

  unsigned long long* p_headerHash = reinterpret_cast<unsigned long long*>(tableMeta);
  //unsigned int* p_tableVersion = reinterpret_cast<unsigned int*>(&tableMeta[8]);
  //int* p_tableFlags = reinterpret_cast<int*>(&tableMeta[12]);
  //unsigned long long* p_freeBytes1 = reinterpret_cast<unsigned long long*>(&tableMeta[16]);
  unsigned int* p_tableVersionMax = reinterpret_cast<unsigned int*>(&tableMeta[24]);
  int* p_nrOfCols = reinterpret_cast<int*>(&tableMeta[28]);
  //unsigned long long* primaryChunkSetLoc = reinterpret_cast<unsigned long long*>(&tableMeta[32]);
  int* p_keyLength = reinterpret_cast<int*>(&tableMeta[40]);
 
  // check header hash
  unsigned long long hHash = XXH64(&tableMeta[8], TABLE_META_SIZE - 8, FST_HASH_SEED);  // skip first 8 bytes (hash value itself)

  if (hHash != *p_headerHash)
  {
    myfile.close();
    throw(runtime_error(FSTERROR_NON_FST_FILE));
  }

  // Compare file version with current
  if (*p_tableVersionMax > FST_VERSION)
  {
    myfile.close();
    throw(runtime_error(FSTERROR_UPDATE_FST));
  }

  keyLength          = *p_keyLength;
  nrOfColsFirstChunk = *p_nrOfCols;

  return *p_tableVersionMax;
}


inline void SetKeyIndex(vector<int> &keyIndex, int keyLength, int nrOfSelect, int* keyColPos, int* colIndex)
{
  for (int i = 0; i < keyLength; ++i)
  {
    int colSel = 0;

    for (; colSel < nrOfSelect; ++colSel)
    {
      if (keyColPos[i] == colIndex[colSel])  // key present in result
      {
        keyIndex.push_back(colSel);
        break;
      }
    }

    // key column not selected
    if (colSel == nrOfSelect) return;
  }
}


/**
 * \brief Write a dataset to a fst file
 * \param fstTable interface to a dataset
 * \param compress compression factor in the range 0 - 100 
 */
void FstStore::fstWrite(IFstTable &fstTable, int compress) const
{
  // Meta on dataset
  int nrOfCols =  fstTable.NrOfColumns();  // number of columns in table
  int keyLength = fstTable.NrOfKeys();  // number of key columns in table

  if (nrOfCols == 0)
  {
    throw(runtime_error("Your dataset needs at least one column."));
  }


  unsigned long long tableHeaderSize    = 44;
  unsigned long long keyIndexHeaderSize = 0;

  if (keyLength != 0)
  {
    keyIndexHeaderSize = 4 * (keyLength + 2);  // size of key index vector and hash
  }

  unsigned long long chunksetHeaderSize = CHUNKSET_HEADER_SIZE + 8 * nrOfCols;
  unsigned long long colNamesHeaderSize = 24;

  // size of fst file header
  unsigned long long metaDataSize = tableHeaderSize + keyIndexHeaderSize + chunksetHeaderSize + colNamesHeaderSize;
  char * metaDataBlock             = new char[metaDataSize];  // fst metadata


  // Table header [node A] [size: 44]

  unsigned long long* p_headerHash        = reinterpret_cast<unsigned long long*>(metaDataBlock);
  unsigned int* p_tableVersion            = reinterpret_cast<unsigned int*>(&metaDataBlock[8]);
  int* p_tableFlags                       = reinterpret_cast<int*>(&metaDataBlock[12]);
  unsigned long long* p_freeBytes1        = reinterpret_cast<unsigned long long*>(&metaDataBlock[16]);
  unsigned int* p_tableVersionMax         = reinterpret_cast<unsigned int*>(&metaDataBlock[24]);
  int* p_nrOfCols                         = reinterpret_cast<int*>(&metaDataBlock[28]);
  unsigned long long* primaryChunkSetLoc  = reinterpret_cast<unsigned long long*>(&metaDataBlock[32]);
  int* p_keyLength                        = reinterpret_cast<int*>(&metaDataBlock[40]);

  // Key index vector (only needed when keyLength > 0) [attached leaf of A] [size: 8 + 4 * keyLength]

  unsigned long long* p_keyIndexHash      = reinterpret_cast<unsigned long long*>(&metaDataBlock[44]);
  int* keyColPos                          = reinterpret_cast<int*>(&metaDataBlock[52]);

  // Chunkset header [node C, free leaf of A or other chunkset header] [size: 76 + 8 * nrOfCols]

  unsigned int offset = tableHeaderSize + keyIndexHeaderSize;
  unsigned long long* p_chunksetHash      = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset]);
  unsigned int* p_chunksetHeaderVersion   = reinterpret_cast<unsigned int*>(&metaDataBlock[offset + 8]);
  int* p_chunksetFlags                    = reinterpret_cast<int*>(&metaDataBlock[offset + 12]);
  unsigned long long* p_freeBytes2        = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 16]);
  unsigned long long* p_freeBytes3        = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 24]);
  unsigned long long* p_colNamesPos       = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 32]);

  unsigned long long* p_nextHorzChunkSet  = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 40]);
  unsigned long long* p_primChunksetIndex = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 48]);
  unsigned long long* p_secChunksetIndex  = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 56]);
  unsigned long long* p_nrOfRows          = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 64]);
  int* p_nrOfChunksetCols                 = reinterpret_cast<int*>(&metaDataBlock[offset + 72]);

  unsigned short int* colAttributeTypes   = reinterpret_cast<unsigned short int*>(&metaDataBlock[offset + 76]);
  unsigned short int* colTypes            = reinterpret_cast<unsigned short int*>(&metaDataBlock[offset + 76 + 2 * nrOfCols]);
  unsigned short int* colBaseTypes        = reinterpret_cast<unsigned short int*>(&metaDataBlock[offset + 76 + 4 * nrOfCols]);
  unsigned short int* colScales           = reinterpret_cast<unsigned short int*>(&metaDataBlock[offset + 76 + 6 * nrOfCols]);

  // Column names [leaf to C]  [size: 24 + x]

  offset = offset + chunksetHeaderSize;
  unsigned long long* p_colNamesHash      = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset]);
  unsigned int* p_colNamesVersion         = reinterpret_cast<unsigned int*>(&metaDataBlock[offset + 8]);
  int* p_colNamesFlags                    = reinterpret_cast<int*>(&metaDataBlock[offset + 12]);
  unsigned long long* p_freeBytes4        = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 16]);


  // Set table header parameters
  
  *p_tableVersion          = FST_VERSION;
  *p_tableFlags            = 0;
  *p_freeBytes1            = 0;
  *p_tableVersionMax       = FST_VERSION;

  *p_nrOfCols              = nrOfCols;
  *primaryChunkSetLoc      = 52 + 4 * keyLength;
  *p_keyLength             = keyLength;

  *p_headerHash = XXH64(&metaDataBlock[8], tableHeaderSize - 8, FST_HASH_SEED);

  // Set key index if present

  if (keyLength != 0)
  {
    fstTable.GetKeyColumns(keyColPos);
    *p_keyIndexHash = XXH64(&metaDataBlock[tableHeaderSize + 8], keyIndexHeaderSize - 8, FST_HASH_SEED);
  }

  *p_chunksetHeaderVersion = FST_VERSION;
  *p_chunksetFlags         = 0;
  *p_freeBytes2            = 0;
  *p_freeBytes3            = 0;
  *p_colNamesPos           = 0;
  *p_nextHorzChunkSet      = 0;
  *p_primChunksetIndex     = 0;
  *p_secChunksetIndex      = 0;

  unsigned long long nrOfRows = fstTable.NrOfRows();
  *p_nrOfRows              = nrOfRows;
  *p_nrOfChunksetCols      = nrOfCols;

  // Set column names header

  *p_colNamesVersion       = FST_VERSION;
  *p_colNamesFlags         = 0;
  *p_freeBytes4            = 0;

  if (nrOfRows == 0)
  {
    delete[] metaDataBlock;
    throw(runtime_error(FSTERROR_NO_DATA));
  }

  *p_colNamesHash = XXH64(p_colNamesVersion, colNamesHeaderSize - 8, FST_HASH_SEED);


  // Open file with default buffer
  ofstream myfile;
  myfile.open(fstFile.c_str(), ios::out | ios::binary);  // write stream only

  if (myfile.fail())
  {
    delete[] metaDataBlock;
    myfile.close();
    throw(runtime_error(FSTERROR_ERROR_OPEN_WRITE));
  }

  // Write table meta information
  myfile.write(metaDataBlock, metaDataSize);  // table meta data

  // Serialize column names
  IStringWriter* blockRunner = fstTable.GetColNameWriter();
  fdsWriteCharVec_v6(myfile, blockRunner, 0, StringEncoding::NATIVE);   // column names
  delete blockRunner;


  // Size of chunkset index header plus data chunk header
  unsigned long long chunkIndexSize = CHUNK_INDEX_SIZE + DATA_INDEX_SIZE + 8 * nrOfCols;
  char* chunkIndex = new char[chunkIndexSize];

  // Chunkset data index [node D, leaf of C] [size: 96]

  unsigned long long* p_chunkIndexHash = reinterpret_cast<unsigned long long*>(chunkIndex);
  unsigned int* p_chunkIndexVersion    = reinterpret_cast<unsigned int*>(&chunkIndex[8]);
  int* p_chunkIndexFlags               = reinterpret_cast<int*>(&chunkIndex[12]);
  unsigned long long* p_freeBytes5     = reinterpret_cast<unsigned long long*>(&chunkIndex[16]);
  unsigned short int* p_nrOfChunkSlots = reinterpret_cast<unsigned short int*>(&chunkIndex[24]);
  unsigned short int* p_freeBytes6     = reinterpret_cast<unsigned short int*>(&chunkIndex[26]);
  unsigned long long* p_chunkPos       = reinterpret_cast<unsigned long long*>(&chunkIndex[32]);
  unsigned long long* p_chunkRows      = reinterpret_cast<unsigned long long*>(&chunkIndex[64]);

  // Chunk data header [node E, leaf of D] [size: 24 + 8 * nrOfCols]

  unsigned long long* p_chunkDataHash  = reinterpret_cast<unsigned long long*>(&chunkIndex[96]);
  unsigned int* p_chunkDataVersion     = reinterpret_cast<unsigned int*>(&chunkIndex[104]);
  int* p_chunkDataFlags                = reinterpret_cast<int*>(&chunkIndex[108]);
  unsigned long long* p_freeBytes7     = reinterpret_cast<unsigned long long*>(&chunkIndex[112]);
  unsigned long long* positionData     = reinterpret_cast<unsigned long long*>(&chunkIndex[120]);  // column position index


  // Set chunkset data index header parameters

  *p_chunkIndexVersion = FST_VERSION;
  *p_chunkIndexFlags   = 0;
  *p_freeBytes5       = 0;
  *p_nrOfChunkSlots   = 4;
   p_freeBytes6[0]    = p_freeBytes6[1] = p_freeBytes6[2] = 0;
  *p_chunkRows        = nrOfRows;

  // Set data chunk header parameters

  *p_chunkDataVersion = FST_VERSION;
  *p_chunkDataFlags   = 0;
  *p_freeBytes7       = 0;


  // Row and column meta data
  myfile.write(chunkIndex, chunkIndexSize);   // file positions of column data


  // column data
  for (int colNr = 0; colNr < nrOfCols; ++colNr)
  {
    positionData[colNr] = myfile.tellp();  // current location
  	FstColumnAttribute colAttribute;
  	std::string annotation = "";
    short int scale = 0;

  	// get type and add annotation
    FstColumnType colType = fstTable.ColumnType(colNr, colAttribute, scale, annotation);

    colBaseTypes[colNr] = static_cast<unsigned short int>(colType);
  	colAttributeTypes[colNr] = static_cast<unsigned short int>(colAttribute);
    colScales[colNr] = scale;

    switch (colType)
    {
      case FstColumnType::CHARACTER:
      {
        colTypes[colNr] = 6;
     		IStringWriter* stringWriter = fstTable.GetStringWriter(colNr);
        fdsWriteCharVec_v6(myfile, stringWriter, compress, stringWriter->Encoding());   // column names
     		delete stringWriter;
        break;
      }

      case FstColumnType::FACTOR:
      {
        colTypes[colNr] = 7;
        int* intP = fstTable.GetIntWriter(colNr);  // level values pointer
     		IStringWriter* stringWriter = fstTable.GetLevelWriter(colNr);
        fdsWriteFactorVec_v7(myfile, intP, stringWriter, nrOfRows, compress, stringWriter->Encoding(), annotation);
	      delete stringWriter;
        break;
      }

      case FstColumnType::INT_32:
      {
        colTypes[colNr] = 8;
        int* intP = fstTable.GetIntWriter(colNr);
        fdsWriteIntVec_v8(myfile, intP, nrOfRows, compress, annotation);
        break;
      }

      case FstColumnType::DOUBLE_64:
      {
        colTypes[colNr] = 9;
        double* doubleP = fstTable.GetDoubleWriter(colNr);
        fdsWriteRealVec_v9(myfile, doubleP, nrOfRows, compress, annotation);
        break;
      }

      case FstColumnType::BOOL_2:
      {
        colTypes[colNr] = 10;
        int* intP = fstTable.GetLogicalWriter(colNr);
        fdsWriteLogicalVec_v10(myfile, intP, nrOfRows, compress, annotation);
        break;
      }

      case FstColumnType::INT_64:
      {
        colTypes[colNr] = 11;
        long long* intP = fstTable.GetInt64Writer(colNr);
        fdsWriteInt64Vec_v11(myfile, intP, nrOfRows, compress, annotation);
        break;
      }

	  case FstColumnType::BYTE:
	  {
		  colTypes[colNr] = 12;
		  char* byteP = fstTable.GetByteWriter(colNr);
		  fdsWriteByteVec_v12(myfile, byteP, nrOfRows, compress, annotation);
		  break;
	  }

    default:
        delete[] metaDataBlock;
        delete[] chunkIndex;
        myfile.close();
        throw(runtime_error("Unknown type found in column."));
    }
  }

  // update chunk position data
  *p_chunkPos = positionData[0] - 8 * nrOfCols - DATA_INDEX_SIZE;

  // Calculate header hashes
  *p_chunksetHash = XXH64(&metaDataBlock[tableHeaderSize + keyIndexHeaderSize + 8], chunksetHeaderSize - 8, FST_HASH_SEED);
  *p_chunkIndexHash = XXH64(&chunkIndex[8], CHUNK_INDEX_SIZE - 8, FST_HASH_SEED);

  myfile.seekp(0);
  myfile.write(metaDataBlock, metaDataSize);  // table header

  *p_chunkDataHash = XXH64(&chunkIndex[CHUNK_INDEX_SIZE + 8], chunkIndexSize - (CHUNK_INDEX_SIZE + 8), FST_HASH_SEED);

  myfile.seekp(*p_chunkPos - CHUNK_INDEX_SIZE);
  myfile.write(chunkIndex, chunkIndexSize);  // vertical chunkset index and positiondata

  // cleanup
  delete[] metaDataBlock;
  delete[] chunkIndex;

  // Check file status only here for performance.
  // Any error that was generated earlier will result in a fail here.
  if (myfile.fail())
  {
	  myfile.close();

	  throw(runtime_error("There was an error during the write operation, fst file might be corrupted. Please check available disk space and access rights."));
  }

  myfile.close();
}


void FstStore::fstMeta(IColumnFactory* columnFactory)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  myfile.open(fstFile.c_str(), ios::in | ios::binary);

  if (myfile.fail())
  {
    myfile.close();
    throw(runtime_error(FSTERROR_ERROR_OPENING_FILE));
  }

  // Read variables from fst file header and check header hash
  version = ReadHeader(myfile, keyLength, nrOfCols);


  unsigned long long keyIndexHeaderSize = 0;

  if (keyLength != 0)
  {
    keyIndexHeaderSize = 4 * (keyLength + 2);  // size of key index vector and hash
  }

  unsigned long long chunksetHeaderSize = CHUNKSET_HEADER_SIZE + 8 * nrOfCols;
  unsigned long long colNamesHeaderSize = 24;
  unsigned long long metaSize = keyIndexHeaderSize + chunksetHeaderSize + colNamesHeaderSize;

  // Read format headers
  metaDataBlock = new char[metaSize];
  myfile.read(metaDataBlock, metaSize);

  if (keyLength != 0)
  {
    keyColPos = reinterpret_cast<int*>(&metaDataBlock[8]);  // equals nullptr if there are no keys

    unsigned long long* p_keyIndexHash = reinterpret_cast<unsigned long long*>(metaDataBlock);
    unsigned long long hHash = XXH64(&metaDataBlock[8], keyIndexHeaderSize - 8, FST_HASH_SEED);

    if (*p_keyIndexHash != hHash)
    {
      delete[] metaDataBlock;
      metaDataBlock = nullptr;
      myfile.close();
      throw(runtime_error(FSTERROR_DAMAGED_HEADER));
    }
  }

  // Chunkset header [node C, free leaf of A or other chunkset header] [size: 76 + 8 * nrOfCols]

  unsigned long long* p_chunksetHash = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize]);
  //unsigned int* p_chunksetHeaderVersion = reinterpret_cast<unsigned int*>(&metaDataBlock[keyIndexHeaderSize + 8]);
  //int* p_chunksetFlags = reinterpret_cast<int*>(&metaDataBlock[keyIndexHeaderSize + 12]);
  //unsigned long long* p_freeBytes2 = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 16]);
  //unsigned long long* p_freeBytes3 = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 24]);
  //unsigned long long* p_colNamesPos = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 32]);

  //unsigned long long* p_nextHorzChunkSet = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 40]);
  //unsigned long long* p_primChunksetIndex = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 48]);
  //unsigned long long* p_secChunksetIndex = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 56]);
  p_nrOfRows = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 64]);
  //int* p_nrOfChunksetCols = reinterpret_cast<int*>(&metaDataBlock[keyIndexHeaderSize + 72]);

  colAttributeTypes = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76]);
  colTypes = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 2 * nrOfCols]);
  colBaseTypes = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 4 * nrOfCols]);
  colScales = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 6 * nrOfCols]);

  unsigned long long chunksetHash = XXH64(&metaDataBlock[keyIndexHeaderSize + 8], chunksetHeaderSize - 8, FST_HASH_SEED);
  if (*p_chunksetHash != chunksetHash)
  {
    delete[] metaDataBlock;
    metaDataBlock = nullptr;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_HEADER));
  }

  // Column names header

  unsigned long long offset = keyIndexHeaderSize + chunksetHeaderSize;
  unsigned long long* p_colNamesHash = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset]);
  //unsigned int* p_colNamesVersion = reinterpret_cast<unsigned int*>(&metaDataBlock[offset + 8]);
  //int* p_colNamesFlags = reinterpret_cast<int*>(&metaDataBlock[offset + 12]);
  //unsigned long long* p_freeBytes4 = reinterpret_cast<unsigned long long*>(&metaDataBlock[16]);

  unsigned long long colNamesHash = XXH64(&metaDataBlock[offset + 8], colNamesHeaderSize - 8, FST_HASH_SEED);
  if (*p_colNamesHash != colNamesHash)
  {
    delete[] metaDataBlock;
    metaDataBlock = nullptr;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_HEADER));
  }

  // Read column names
  unsigned long long colNamesOffset = metaSize + TABLE_META_SIZE;

  blockReader = columnFactory->CreateStringColumn(nrOfCols, FstColumnAttribute::NONE);
  fdsReadCharVec_v6(myfile, blockReader, colNamesOffset, 0, static_cast<unsigned int>(nrOfCols), static_cast<unsigned int>(nrOfCols));

  // cleanup
  metaDataBlock = nullptr;
  myfile.close();
}


void FstStore::fstRead(IFstTable &tableReader, IStringArray* columnSelection, long long startRow, long long endRow,
  IColumnFactory* columnFactory, vector<int> &keyIndex, IStringArray* selectedCols)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  myfile.open(fstFile.c_str(), ios::in | ios::binary);  // only nead an input stream reader

  if (myfile.fail())
  {
    myfile.close();
    throw(runtime_error(FSTERROR_ERROR_OPENING_FILE));
  }

  int keyLength;
  version = ReadHeader(myfile, keyLength, nrOfCols);

  unsigned long long keyIndexHeaderSize = 0;

  if (keyLength != 0)
  {
    keyIndexHeaderSize = 4 * (keyLength + 2);  // size of key index vector and hash
  }

  unsigned long long chunksetHeaderSize = CHUNKSET_HEADER_SIZE + 8 * nrOfCols;
  unsigned long long colNamesHeaderSize = 24;
  unsigned long long metaSize = keyIndexHeaderSize + chunksetHeaderSize + colNamesHeaderSize;

  // Read format headers
  char* metaDataBlock = new char[metaSize];
  myfile.read(metaDataBlock, metaSize);

  int* keyColPos = reinterpret_cast<int*>(&metaDataBlock[8]);  // TODO: why not unsigned ?

  if (keyLength != 0)
  {
    unsigned long long* p_keyIndexHash = reinterpret_cast<unsigned long long*>(metaDataBlock);
    unsigned long long hHash = XXH64(&metaDataBlock[8], keyIndexHeaderSize - 8, FST_HASH_SEED);

    if (*p_keyIndexHash != hHash)
    {
      delete[] metaDataBlock;
      myfile.close();
      throw(runtime_error(FSTERROR_DAMAGED_HEADER));
    }
  }

  // Chunkset header [node C, free leaf of A or other chunkset header] [size: 76 + 8 * nrOfCols]
  
  unsigned long long* p_chunksetHash      = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize]);
  //unsigned int* p_chunksetHeaderVersion   = reinterpret_cast<unsigned int*>(&metaDataBlock[keyIndexHeaderSize + 8]);
  //int* p_chunksetFlags                    = reinterpret_cast<int*>(&metaDataBlock[keyIndexHeaderSize + 12]);
  //unsigned long long* p_freeBytes2        = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 16]);
  //unsigned long long* p_freeBytes3        = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 24]);
  //unsigned long long* p_colNamesPos       = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 32]);

  //unsigned long long* p_nextHorzChunkSet  = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 40]);
  //unsigned long long* p_primChunksetIndex = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 48]);
  //unsigned long long* p_secChunksetIndex  = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 56]);
  //unsigned long long* p_nrOfRows          = reinterpret_cast<unsigned long long*>(&metaDataBlock[keyIndexHeaderSize + 64]);
  //int* p_nrOfChunksetCols                 = reinterpret_cast<int*>(&metaDataBlock[keyIndexHeaderSize + 72]);

  unsigned short int* colAttributeTypes   = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76]);
  unsigned short int* colTypes            = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 2 * nrOfCols]);
  //unsigned short int* colBaseTypes        = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 4 * nrOfCols]);
  unsigned short int* colScales           = reinterpret_cast<unsigned short int*>(&metaDataBlock[keyIndexHeaderSize + 76 + 6 * nrOfCols]);

  unsigned long long chunksetHash = XXH64(&metaDataBlock[keyIndexHeaderSize + 8], chunksetHeaderSize - 8, FST_HASH_SEED);
  if (*p_chunksetHash != chunksetHash)
  {
    delete[] metaDataBlock;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_HEADER));
  }

  // Column names header

  unsigned long long offset          = keyIndexHeaderSize + chunksetHeaderSize;
  unsigned long long* p_colNamesHash = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset]);
  //unsigned int* p_colNamesVersion    = reinterpret_cast<unsigned int*>(&metaDataBlock[offset + 8]);
  //int* p_colNamesFlags               = reinterpret_cast<int*>(&metaDataBlock[offset + 12]);
  //unsigned long long* p_freeBytes4   = reinterpret_cast<unsigned long long*>(&metaDataBlock[offset + 16]);

  unsigned long long colNamesHash = XXH64(&metaDataBlock[offset + 8], colNamesHeaderSize - 8, FST_HASH_SEED);
  if (*p_colNamesHash != colNamesHash)
  {
    delete[] metaDataBlock;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_HEADER));
  }

  // Column names

  unsigned long long colNamesOffset = metaSize + TABLE_META_SIZE;
  blockReader = columnFactory->CreateStringColumn(nrOfCols, FstColumnAttribute::NONE);
  fdsReadCharVec_v6(myfile, blockReader, colNamesOffset, 0, static_cast<unsigned int>(nrOfCols), static_cast<unsigned int>(nrOfCols));

  // Size of chunkset index header plus data chunk header
  unsigned long long chunkIndexSize    = CHUNK_INDEX_SIZE + DATA_INDEX_SIZE + 8 * nrOfCols;
  char* chunkIndex                     = new char[chunkIndexSize];

  myfile.read(chunkIndex, chunkIndexSize);

  // Chunk index [node D, leaf of C] [size: 96]

  unsigned long long* p_chunkIndexHash = reinterpret_cast<unsigned long long*>(chunkIndex);
  //unsigned int* p_chunkIndexVersion    = reinterpret_cast<unsigned int*>(&chunkIndex[8]);
  //int* p_chunkIndexFlags               = reinterpret_cast<int*>(&chunkIndex[12]);
  //unsigned long long* p_freeBytes5     = reinterpret_cast<unsigned long long*>(&chunkIndex[16]);
  //unsigned short int* p_nrOfChunkSlots = reinterpret_cast<unsigned short int*>(&chunkIndex[24]);
  //unsigned short int* p_freeBytes6     = reinterpret_cast<unsigned short int*>(&chunkIndex[26]);
  //unsigned long long* p_chunkPos       = reinterpret_cast<unsigned long long*>(&chunkIndex[32]);
  unsigned long long* p_chunkRows      = reinterpret_cast<unsigned long long*>(&chunkIndex[64]);

  // Chunk data header [node E, leaf of D] [size: 24 + 8 * nrOfCols]

  unsigned long long* p_chunkDataHash  = reinterpret_cast<unsigned long long*>(&chunkIndex[96]);
  //unsigned int* p_chunkDataVersion     = reinterpret_cast<unsigned int*>(&chunkIndex[104]);
  //int* p_chunkDataFlags                = reinterpret_cast<int*>(&chunkIndex[108]);
  //unsigned long long* p_freeBytes7     = reinterpret_cast<unsigned long long*>(&chunkIndex[112]);
  unsigned long long* positionData     = reinterpret_cast<unsigned long long*>(&chunkIndex[120]);  // column position index


  // Check chunk hashes

  unsigned long long chunkIndexHash = XXH64(&chunkIndex[8], CHUNK_INDEX_SIZE - 8, FST_HASH_SEED);

  if (*p_chunkIndexHash != chunkIndexHash)
  {
    delete[] metaDataBlock;
    delete[] chunkIndex;
    delete blockReader;
    blockReader = nullptr;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_CHUNKINDEX));
  }

  unsigned long long chunkDataHash = XXH64(&chunkIndex[CHUNK_INDEX_SIZE + 8], chunkIndexSize - (CHUNK_INDEX_SIZE + 8), FST_HASH_SEED);

  if (*p_chunkDataHash != chunkDataHash)
  {
    delete[] metaDataBlock;
    delete[] chunkIndex;
    delete blockReader;
    blockReader = nullptr;
    myfile.close();
    throw(runtime_error(FSTERROR_DAMAGED_CHUNKINDEX));
  }


  // Read block positions
  unsigned long long* blockPos = positionData;


  // Determine column selection
  int *colIndex;
  int nrOfSelect;

  if (columnSelection == nullptr)
  {
    colIndex = new int[nrOfCols];

    for (int colNr = 0; colNr < nrOfCols; ++colNr)
    {
      colIndex[colNr] = colNr;
    }
    nrOfSelect = nrOfCols;
  }
  else  // determine column numbers of column names
  {
    nrOfSelect = columnSelection->Length();
    colIndex = new int[nrOfSelect];
    for (int colSel = 0; colSel < nrOfSelect; ++colSel)
    {
      int equal = -1;
      const char* str1 = columnSelection->GetElement(colSel);

      for (int colNr = 0; colNr < nrOfCols; ++colNr)
      {
        const char* str2 = blockReader->GetElement(colNr);
        if (strcmp(str1, str2) == 0)
        {
          equal = colNr;
          break;
        }
      }

      if (equal == -1)
      {
        delete[] metaDataBlock;
        delete[] colIndex;
        delete[] chunkIndex;
        delete blockReader;
        blockReader = nullptr;
        myfile.close();
        throw(runtime_error("Selected column not found."));
      }

      colIndex[colSel] = equal;
    }
  }


  // Check range of selected rows
  long long firstRow = startRow - 1;
  unsigned long long nrOfRows = *p_chunkRows;  // TODO: check for row numbers > INT_MAX !!!

  if (firstRow >= static_cast<long long>(nrOfRows) || firstRow < 0)
  {
    delete[] metaDataBlock;
    delete[] colIndex;
    delete[] chunkIndex;
    delete blockReader;
    blockReader = nullptr;
    myfile.close();

    if (firstRow < 0)
    {
      throw(runtime_error("Parameter fromRow should have a positive value."));
    }

    throw(runtime_error("Row selection is out of range."));
  }

  long long length = nrOfRows - firstRow;


  // Determine vector length
  if (endRow != -1)
  {
    if (static_cast<long long>(endRow) <= firstRow)
    {
      delete[] metaDataBlock;
      delete[] colIndex;
      delete[] chunkIndex;
      delete blockReader;
      blockReader = nullptr;
      myfile.close();
      throw(runtime_error("Incorrect row range specified."));
    }

    length = min(endRow - firstRow, static_cast<long long>(nrOfRows) - firstRow);
  }

  tableReader.InitTable(nrOfSelect, length);

  for (int colSel = 0; colSel < nrOfSelect; ++colSel)
  {
    int colNr = colIndex[colSel];

    if (colNr < 0 || colNr >= nrOfCols)
    {
      delete[] metaDataBlock;
      delete[] colIndex;
      delete[] chunkIndex;
      delete blockReader;
      blockReader = nullptr;
      myfile.close();
      throw(runtime_error("Column selection is out of range."));
    }

    unsigned long long pos = blockPos[colNr];
    short int scale = colScales[colNr];

    switch (colTypes[colNr])
    {
    // Character vector
      case 6:
      {
        IStringColumn* stringColumn = columnFactory->CreateStringColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]));
        fdsReadCharVec_v6(myfile, stringColumn, pos, firstRow, length, nrOfRows);
        tableReader.SetStringColumn(stringColumn, colSel);
        delete stringColumn;
        break;
      }

      // Integer vector
      case 8:
      {
        IIntegerColumn* integerColumn = columnFactory->CreateIntegerColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]), scale);
        std::string annotation = "";
        fdsReadIntVec_v8(myfile, integerColumn->Data(), pos, firstRow, length, nrOfRows, annotation);
        tableReader.SetIntegerColumn(integerColumn, colSel, annotation);
        delete integerColumn;
        break;
      }

      // Double vector
      case 9:
      {
        IDoubleColumn* doubleColumn = columnFactory->CreateDoubleColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]), scale);
        std::string annotation = "";
        fdsReadRealVec_v9(myfile, doubleColumn->Data(), pos, firstRow, length, nrOfRows, annotation);
        tableReader.SetDoubleColumn(doubleColumn, colSel, annotation);
        delete doubleColumn;
        break;
      }

      // Logical vector
      case 10:
      {
        ILogicalColumn* logicalColumn = columnFactory->CreateLogicalColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]));
        fdsReadLogicalVec_v10(myfile, logicalColumn->Data(), pos, firstRow, length, nrOfRows);
        tableReader.SetLogicalColumn(logicalColumn, colSel);
        delete logicalColumn;
        break;
      }

      // Factor vector
      case 7:
      {
        IFactorColumn* factorColumn = columnFactory->CreateFactorColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]));
        fdsReadFactorVec_v7(myfile, factorColumn->Levels(), factorColumn->LevelData(), pos, firstRow, length, nrOfRows);
        tableReader.SetFactorColumn(factorColumn, colSel);
        delete factorColumn;
        break;
      }

	  // integer64 vector
	  case 11:
	  {
	    IInt64Column* int64Column = columnFactory->CreateInt64Column(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]), scale);
      fdsReadInt64Vec_v11(myfile, int64Column->Data(), pos, firstRow, length, nrOfRows);
	    tableReader.SetInt64Column(int64Column, colSel);
	    delete int64Column;
	    break;
	  }

	  // byte vector
	  case 12:
	  {
		  IByteColumn* byteColumn = columnFactory->CreateByteColumn(length, static_cast<FstColumnAttribute>(colAttributeTypes[colNr]));
		  fdsReadByteVec_v12(myfile, byteColumn->Data(), pos, firstRow, length, nrOfRows);
		  tableReader.SetByteColumn(byteColumn, colSel);
		  delete byteColumn;
		  break;
	  }

    default:
      delete[] metaDataBlock;
      delete[] colIndex;
      delete[] chunkIndex;
      delete blockReader;
      blockReader = nullptr;
      myfile.close();
      throw(runtime_error("Unknown type found in column."));
    }
  }

  // delete blockReaderStrVec;

  myfile.close();

  // Key index
  SetKeyIndex(keyIndex, keyLength, nrOfSelect, keyColPos, colIndex);

  selectedCols->AllocateArray(nrOfSelect);

  // Only when keys are present in result set, TODO: compute using C++ only !!!
  for (int i = 0; i < nrOfSelect; ++i)
  {
    selectedCols->SetElement(i, blockReader->GetElement(colIndex[i]));
  }

  delete[] metaDataBlock;
  delete[] colIndex;
  delete[] chunkIndex;
}
