#include "ebSmartQTFilter.hxx"
#include "midas.h"
#include <string.h>
#include <iostream>
#include <algorithm>

#define NUM_SQ_WORDS 5

ebSmartQTFilter::ebSmartQTFilter() {
  fDebugKeepZLECopy = false;// true => original ZLXX, filtered ZFXX; false => filtered ZLXX
  fDebugKeepSQCopy = false; // true => original SQXX, filtered SFXX; false => filtered SQXX
  fDebugKeepW4Copy = false; // true => original W4XX, filtered F4XX; false => filtered W4XX
  fDebugKeepMN = false;
  fSaveAllQT = true;
  fEventCount = 0;

  fSaveAllSmartQT = false;
  fSaveSmartQTEvenIfSavingZLE = true;

  // Below are filled from ODB
  fEnableV1720Filtering = false;
  fV1720SPEConfidenceThreshold = 0;

  fEnableV1740Filtering = true;
  fSaveV1740Threshold = 4096;

  for (int i = 0; i < 256; i++) {
    fV1720V1740Map[i] = -1;
  }

  for (int i = 0; i < 256; i++) {
    fAnalysisResultsFilterZLE.push_back(std::vector<int>());
  }
}

bool ebSmartQTFilter::ReadODB(HNDLE& hDB) {
  HNDLE heb;
  db_find_key(hDB, 0, "/Equipment/EBuilder/Settings", &heb);

  if (!heb) {
    cm_msg(MERROR,"ReadODB","Cannot read EBuilder settings from ODB");
    return false;
  }

  INT size_bool = sizeof(fEnableV1740Filtering);
  if (db_get_value(hDB, heb, "Enable V1740 Filtering", &fEnableV1740Filtering, &size_bool, TID_BOOL, TRUE) != DB_SUCCESS) {
    cm_msg(MERROR,"ReadODB","Failed to read whether to enable V1740 filtering in EBuilder");
    return false;
  }

  INT size_int = sizeof(fSaveV1740Threshold);
  if (db_get_value(hDB, heb, "V1720 Threshold To Save V1740", &fSaveV1740Threshold, &size_int, TID_INT, TRUE) != DB_SUCCESS) {
    cm_msg(MERROR,"ReadODB","Failed to read V1720 threshold for V1740 filtering in EBuilder");
    return false;
  }

  size_bool = sizeof(fEnableV1720Filtering);
  if (db_get_value(hDB, heb, "Enable V1720 Filtering", &fEnableV1720Filtering, &size_bool, TID_BOOL, TRUE) != DB_SUCCESS) {
    cm_msg(MERROR,"ReadODB","Failed to read whether to enable V1720 filtering in EBuilder");
    return false;
  }

  size_int = sizeof(fV1720SPEConfidenceThreshold);
  if (db_get_value(hDB, heb, "V1720 SPE Confidence Threshold", &fV1720SPEConfidenceThreshold, &size_int, TID_INT, TRUE) != DB_SUCCESS) {
    cm_msg(MERROR,"ReadODB","Failed to read V1720 SPE threshold for EBuilder");
    return false;
  }

  HNDLE hpmt;
  db_find_key(hDB, 0, "/PMTinfo", &hpmt);

  if (!hpmt) {
    cm_msg(MERROR,"ReadODB","Cannot read V1720-V1740 map from ODB - does /PMTinfo exist?");
    return false;
  }

  INT size_map = sizeof(fV1720V1740Map);
  if (db_get_value(hDB, hpmt, "V17202V1740", &fV1720V1740Map, &size_map, TID_INT, FALSE) != DB_SUCCESS) {
    cm_msg(MERROR,"ReadODB","Cannot read V1720-V1740 map from ODB - does /PMTinfo/V17202V1740 exist?");
    return false;
  }

  return true;
}

void ebSmartQTFilter::AnalyzeBanks(char * src) {
  fEventCount++;

  BANK32 *pbh = NULL;
  DWORD *pdata_b = NULL;
  INT bksize;
  EVENT_HEADER *pevent;
  pevent = (EVENT_HEADER *)src;
  int nQTWords = 0;
  int module = 0;
  int channel = 0;
  int conf = 0;
  int offset = 0;
  int idx = 0;
  int peakV = 0;

  do {
    bksize = bk_iterate32((BANK_HEADER *) (pevent + 1), &pbh, &pdata_b);

    if(bksize){
      if (!strncmp(pbh->name, "MN", 2) && fEnableV1740Filtering) { // Handle "minima" banks
        // The approach here is to check whether this pulse saturated.
        // If so, we'll flag to save the V1740 information for this channel.

        // Get the module number
        std::string sname(pbh->name);
        std::string smodule = sname.substr(2, 2);
        module = atoi(smodule.c_str());

        // Read the packed data - 8 minima in 4 32-bit words (ordered by channel).
        for (int i = 0; i < 4; i++) {
          // We want to flag the *V1740* index rather than the V1720 index,
          // so must convert using the map we read from ODB.
          int idx20 = (module * 8) + (i * 2);
          int idx40 = fV1720V1740Map[idx20];
          unsigned int min1 = ((pdata_b[2+i] >> 16) & 0xFFFF);
          unsigned int min2 = (pdata_b[2+i] & 0xFFFF);

          if (min1 < fSaveV1740Threshold) {
            fAnalysisResultsSaveV1740.insert(idx40);
          }

          idx20++;
          idx40 = fV1720V1740Map[idx20];

          if (min2 < fSaveV1740Threshold) {
            fAnalysisResultsSaveV1740.insert(idx40);
          }
        }
      }

      if (!strncmp(pbh->name, "SQ", 2) && fEnableV1720Filtering) { // Handle Smart QT banks
        // Get the module number
        std::string sname(pbh->name);
        std::string smodule = sname.substr(2, 2);
        module = atoi(smodule.c_str());

        nQTWords = pdata_b[2];
        int previdx = -1;
        int prevoffset = -1;

        for (int w = 3; w < nQTWords + 3; w += NUM_SQ_WORDS) {
          channel = ((pdata_b[w] >> 28) & 0xF);
          conf = ((pdata_b[w+3]) & 0xFF);
          offset = ((pdata_b[w+2] >> 16) & 0xFFFF);
          peakV = ((pdata_b[w] >> 8) & 0xFFF);
          idx = module * 8 + channel;

          if (idx < 0 || idx > 255) {
            // Bad formatting
            continue;
          }

          if (previdx == idx && prevoffset == offset && fAnalysisResultsFilterZLE[idx].size() && fAnalysisResultsFilterZLE[idx].back() == offset) {
            // More than one pulse in a block - don't filter ZLE
            fAnalysisResultsFilterZLE[idx].pop_back();
          } else if (conf > fV1720SPEConfidenceThreshold && conf < 201) {
            // SPE-like - do filter ZLE
            fAnalysisResultsFilterZLE[idx].push_back(offset);
          }

          previdx = idx;
          prevoffset = offset;
        }
      }
    }
  } while (bksize);
}

void ebSmartQTFilter::WriteFilteredBanks(char * dest_pevent, char * src) {
  BANK32 *pbh = NULL;
  DWORD *src_pdata = NULL;
  DWORD *dest_pdata = NULL;
  INT dest_words = 0;
  INT bksize;
  EVENT_HEADER *src_pevent;
  src_pevent = (EVENT_HEADER *)src;

  do {
    bksize = bk_iterate32((BANK_HEADER *) (src_pevent + 1), &pbh, &src_pdata);

    if(bksize){
      if(!strncmp(pbh->name, "ZL", 2)) {
        if (!fEnableV1720Filtering) {
          // Just copy over the ZLE as-is if requested
          bk_copy(dest_pevent, src, pbh->name);
          continue;
        }

        // Otherwise, we need to filter the ZLE bank information.
        std::string sname(pbh->name);
        std::string smodule = sname.substr(2, 2);
        int module = atoi(smodule.c_str());

        // First create a new ZLE bank in the destination. If we're keeping a
        // copy of the original bank (for debugging), we call this new bank
        // ZFXX; otherwise we call it ZLXX.
        char tBankName[5];

        if (fDebugKeepZLECopy) {
          bk_copy(dest_pevent, src, pbh->name);
          snprintf(tBankName, sizeof(tBankName), "ZF%02d", module);
        } else {
          snprintf(tBankName, sizeof(tBankName), "ZL%02d", module);
        }

        bk_create(dest_pevent, tBankName, TID_DWORD, (void **)&dest_pdata);


        // Now get the current ZLE bank data from the source,
        // and do any filtering

        // Copy over the 4 header words.
        // [0] is the size, which we fill later.
        DWORD* dest_size = dest_pdata;
        *dest_pdata++ = src_pdata[0]; // Upper byte is ?; lower 24 bits are total size
        *dest_pdata++ = src_pdata[1]; // chMask
        *dest_pdata++ = src_pdata[2]; // ?
        *dest_pdata++ = src_pdata[3]; // ?
        dest_words = 4;


        bool     chEnabled[8] = {false};        // Channel enabled flag

        uint32_t iCurrentWord;        // Index of current 32-bit word in the ZLE event
        uint32_t chSize_words;        // Size of the current channel in 32-bit words
        uint32_t words_read;          // Number of words read so far for the current channel
        uint32_t iCurrentSample;      // Index of current sample in the channel
        bool     goodData;            // Indicates if the data following the control word must be processed or skipped
        bool     prevGoodData;        //
        uint32_t nStoredSkippedWords; // Number of words to be stored (goodData = true) or skipped (goodData = false)
        int      nEnabledChannels = 0;

        // >>> Figure out channel mapping
        uint32_t chMask = src_pdata[1] & 0xFF;
        for(int channel = 0; channel < 8; ++channel){
          if(chMask & (1<<channel)){
            chEnabled[channel] = true;
            ++nEnabledChannels;
          }
        }


        iCurrentWord = 4;  //Go to first CH0 size word

        for(int channel = 0; channel < 8; channel++) {
          if(!chEnabled[channel]) continue;

          chSize_words = src_pdata[iCurrentWord];// Read size word
          iCurrentSample = 0;                    // Start processing sample 0
          words_read = 1;                        // The size of the "size" word is included in its value
          iCurrentWord++;                        // Go to CH0 control word
          prevGoodData = false;

          // We'll fill the new size of this channel later
          DWORD* dest_chSize_words = dest_pdata;
          int dest_words_ch = 1;
          *dest_pdata++ = 0;
          dest_words++;

          while(words_read < chSize_words){
            goodData = ((src_pdata[iCurrentWord]>>31) & 0x1);           // 0: skip 1: good
            nStoredSkippedWords = (src_pdata[iCurrentWord] & 0xFFFFF);  // stored/skipped words

            if(goodData){
              if (!ShouldKeepZLE(module, channel, iCurrentSample)) {
                // THIS BLOCK HAS BEEN CHOSEN TO BE SKIPPED!
                // For now, we'll just add multiple skip blocks in a row.
                // This can result in multiple bad blocks in a row.
                // In future we could do something cleverer, like:
                /*
                if (prevGoodData) {
                  // Previous good data - need to add a new bad block
                } else {
                  // Previous bad data - can simply extend that bad block
                }
                */

                uint32_t newskip = nStoredSkippedWords; // Highest bit will be 0 => skip
                *dest_pdata++ = newskip;
                iCurrentWord++;
                words_read++;
                dest_words++;
                dest_words_ch++;

                words_read += nStoredSkippedWords;
                iCurrentWord += nStoredSkippedWords;
              } else {
                *dest_pdata++ = src_pdata[iCurrentWord]; // Copy store/skip word
                iCurrentWord++;
                words_read++;
                dest_words++;
                dest_words_ch++;

                // Actual data words are copied over as-is
                for(uint32_t j = 0; j < nStoredSkippedWords; j++){
                  *dest_pdata++ = src_pdata[iCurrentWord];
                  iCurrentWord++;
                  words_read++;
                  dest_words++;
                  dest_words_ch++;
                }

              }
            } else {
              /* Data is bad, copy this word over as-is. */
              *dest_pdata++ = src_pdata[iCurrentWord];
              iCurrentWord++;
              words_read++;
              dest_words++;
              dest_words_ch++;
            }

            iCurrentSample += (nStoredSkippedWords*2); //2 samples per 32-bit word
          }

          *dest_chSize_words = dest_words_ch;
        }

        // We keep the upper byte as it is, but replace the lower
        // 24 bits with our new size.
        *dest_size &= ~0xFFFFFF;
        *dest_size |= (dest_words & 0xFFFFFF);

        // And close the new ZLE bank.
        bk_close(dest_pevent, dest_pdata);
      } else if (!strncmp(pbh->name, "SQ", 2)) {
        if (fSaveAllSmartQT) {
          bk_copy(dest_pevent, src, pbh->name);
          continue;
        }

        // Otherwise we look at filtering the smart QT
        // Get the module number
        std::string sname(pbh->name);
        std::string smodule = sname.substr(2, 2);
        int module = atoi(smodule.c_str());

        // First create a new SQ bank in the destination. If we're keeping a
        // copy of the original bank (for debugging), we call this new bank
        // SFXX; otherwise we call it SQXX.
        char tBankName[5];

        if (fDebugKeepSQCopy) {
          bk_copy(dest_pevent, src, pbh->name);
          snprintf(tBankName, sizeof(tBankName), "SF%02d", module);
        } else {
          snprintf(tBankName, sizeof(tBankName), "SQ%02d", module);
        }

        bk_create(dest_pevent, tBankName, TID_DWORD, (void **)&dest_pdata);

        // Copy over the 3 header words.
        // [0] is the size, which we fill later.
        *dest_pdata++ = src_pdata[0]; // ?
        *dest_pdata++ = src_pdata[1]; // ?
        DWORD* dest_nQTWords = dest_pdata;
        *dest_pdata++ = src_pdata[2]; // ?

        (*dest_nQTWords) = 0;

        int src_nQTWords = src_pdata[2];

        for (int w = 3; w < src_nQTWords + 3; w += NUM_SQ_WORDS) {
          int channel = ((src_pdata[w] >> 28) & 0xF);
          int conf = ((src_pdata[w+3]) & 0xFF);
          int offset = ((src_pdata[w+2] >> 16) & 0xFFFF);

          if (!ShouldKeepZLE(module, channel, offset) || fSaveSmartQTEvenIfSavingZLE) {
            // Not keeping ZLE, so should keep Smart QT.
            // Otherwise we are keeping ZLE so don't need Smart QT.

            for (int o = 0; o < NUM_SQ_WORDS; o++) {
              *dest_pdata++ = src_pdata[w+o];
            }

            *dest_pdata += NUM_SQ_WORDS;
            (*dest_nQTWords) += NUM_SQ_WORDS;
          }
        }

        // And close the new SQ bank.
        bk_close(dest_pevent, dest_pdata);
      } else if (!strncmp(pbh->name, "W4", 2)) {
        if (!fEnableV1740Filtering) {
          bk_copy(dest_pevent, src, pbh->name);
          continue;
        }

        // Get the board number (0-3)
        std::string sname(pbh->name);
        std::string sboard = sname.substr(2, 2);
        int board = atoi(sboard.c_str());

        // First create a new V1740 bank in the destination. If we're keeping a
        // copy of the original bank (for debugging), we call this new bank
        // F4XX; otherwise we call it W4XX.
        char tBankName[5];

        if (fDebugKeepW4Copy) {
          bk_copy(dest_pevent, src, pbh->name);
          snprintf(tBankName, sizeof(tBankName), "F4%02d", board);
        } else {
          snprintf(tBankName, sizeof(tBankName), "W4%02d", board);
        }

        bk_create(dest_pevent, tBankName, TID_DWORD, (void **)&dest_pdata);

        // Filter V1740 information based on results.
        DWORD* dest_size = dest_pdata;
        *dest_pdata++ = src_pdata[0]; // Lower 24 bits are midas data size (we'll set this later)
        DWORD* dest_mask = dest_pdata;
        *dest_pdata++ = src_pdata[1]; // Midas channel group mask
        *dest_pdata++ = src_pdata[2]; // Midas event counter
        *dest_pdata++ = src_pdata[3]; // Midas trigger tag

        int src_size = (src_pdata[0] & 0xFFFFFF);
        int src_group_mask = (src_pdata[1] & 0xFF);
        int dest_words = 4;
        int dest_group_mask = 0;
        int src_word = 4;

        // count the number of active groups
        int src_group_count = 0;
        for (int32_t grp=0; grp<8; ++grp) {
            if (src_group_mask & (1 << grp)) ++src_group_count;
        }
        if (!src_group_count) {
          // No active groups - no filtering to be done
          bk_close(dest_pevent, dest_pdata);
          continue;
        }

        // each chunk of 9 words contains 24 12-bit samples
        // (3 samples for each of the 8 channels)
        int nChunks = (src_size - 4) / (9 * src_group_count);   // number of chunks per group
        if (!nChunks) {
          bk_close(dest_pevent, dest_pdata);
          continue;
        }

        for (int grp=0; grp<8; ++grp) {
            // consider only active groups
            if (!(src_group_mask & (1 << grp))) {
              continue;
            }

            int end_src_word = src_word + nChunks * 9;

            if (ShouldKeepV1740(board, grp)) {
              // Copy the data for this group.
              dest_group_mask |= (1 << grp);
              while (src_word < end_src_word) {
                *dest_pdata++ = src_pdata[src_word++];
                dest_words++;
              }
            } else {
              // Skip ahead to the next group.
              src_word = end_src_word;
            }
        }

        // Update the mask. Only want to update the lower byte of it,
        // leaving the rest in-tact. First clear the lower byte, then
        // set it.
        *dest_mask &= ~0xFF;
        *dest_mask |= (dest_group_mask & 0xFF);

        // Similarly for the size in words - leave the upper byte alone.
        *dest_size &= ~0xFFFFFF;
        *dest_size |= (dest_words & 0xFFFFFF);

        // And close the new V1740 bank.
        bk_close(dest_pevent, dest_pdata);
      } else if (!strncmp(pbh->name, "QT", 2)) {
        if (fSaveAllQT) {
          bk_copy(dest_pevent, src, pbh->name);
        }
      } else if (!strncmp(pbh->name, "MN", 2)) {
        if (fDebugKeepMN) {
          bk_copy(dest_pevent, src, pbh->name);
        }
      } else {
        // Just copy over all other banks (DTRG etc)
        bk_copy(dest_pevent, src, pbh->name);
      }
    }
  } while (bksize);
}

bool ebSmartQTFilter::ShouldKeepZLE(int module, int channel, int offset) {
  int index = module * 8 + channel;
  if (index < 0 || index > 255) return true;
  return (std::find(fAnalysisResultsFilterZLE[index].begin(), fAnalysisResultsFilterZLE[index].end(), offset) == fAnalysisResultsFilterZLE[index].end());
}

bool ebSmartQTFilter::ShouldKeepV1740(int board, int group) {
  // Check whether any of the 8 channels in this group have been
  // flagged for saving.
  for (int channel = group * 8; channel < (group + 1) * 8; channel++) {
    int index = (board * 64) + channel;

    if (fAnalysisResultsSaveV1740.count(index)) {
      return true;
    }
  }

  return false;
}

bool ebSmartQTFilter::ShouldKeepAnyV1740() {
  return fAnalysisResultsSaveV1740.size();
}

void ebSmartQTFilter::ResetV1720Analysis() {
  for (int i = 0; i < 256; i++) {
    fAnalysisResultsFilterZLE[i].clear();
  }
}

void ebSmartQTFilter::ResetV1740Analysis() {
  fAnalysisResultsSaveV1740.clear();
}
