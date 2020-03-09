#include "ebFilterDecision.hxx"

ebFilterDecision::ebFilterDecision() {
  // These will be set at the beginning of each run.
  fLowE = 0;
  fMedE = 0;
  fHighE = 0;
  fFpromptLowE = 0;
  fFpromptMedE = 0;
  fNarrowWindow = 0;
  fWideWindow = 0;
  fNQThresh = 0;
}

void ebFilterDecision::ReadODB(HNDLE& hDB, HNDLE& hsf) {
  INT size = sizeof(fLowE);
  db_get_value(hDB, hsf, "QT summary rebin factor", &fRebinFactor, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Energy thresh low", &fLowE, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Energy thresh med", &fMedE, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Energy thresh high", &fHighE, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Fprompt thresh low", &fFpromptLowE, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Fprompt thresh med", &fFpromptMedE, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Window start offset", &fStartOffset, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Narrow window width", &fNarrowWindow, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Wide window width", &fWideWindow, &size, TID_INT, TRUE);
  db_get_value(hDB, hsf, "Max N_QT to use Q histo", &fNQThresh, &size, TID_INT, TRUE);

  // Convert ns values read from ODB into number of QT summary bins
  fStartOffset /= (4 * fRebinFactor);
  fNarrowWindow /= (4 * fRebinFactor);
  fWideWindow /= (4 * fRebinFactor);
}

/**
 * \brief   Analyze the Q vs T histogram and make filter decision
 *
 * Method will analyze the input Q vs T histogram, which is produced in the threads from ZLE banks.
 * It will then decide whether to save the ZLE and/or QT banks.
 *
 * \param   [in] Qhisto      Vector of integers that contains the charge for time from the ZLE banks.
 *                             Each QT block is represented as 1 number (at the minimum of the pulse).
 * \param   [in] Nhisto      Same a Qhisto, but not charge-weighted (simply number of QT blocks at time T).
 * \param   [in/out]  pevent Pointer to the current event (so that we can add a summary bank to it)
 * \param   [out]  saveZLE   Do we save ZLE information for this event?
 * \param   [out]  saveQT    Do we save QT information for this event?
 *                       
 * \return        
 */
bool ebFilterDecision::FPromptDecision(std::vector<int>& Qhisto, std::vector<int>& Nhisto, char *pevent, bool &saveZLE, bool &saveQT){

  DWORD total = 0;
  DWORD narrow = 0;
  DWORD wide = 0;
  BYTE efpbin = BOX_NOTSET; // Will only use 4 bits of this
  WORD time = 0;

  // Energy/Fprompt logic comes from Kamal's study.
  // To find the trigger time, we do not weight each QT block by charge (as this
  // is easily fooled by large afterpulses). We use 16ns bins to smooth out
  // the distributions.
  // To calculate E and Fprompt (based on the time found above), we do weight
  // each QT block by charge. Again, we use 16ns blocks.
  // The windows start 'fStartOffset' bins before the trigger time.
  // The narrow window is 'fNarrowWindow' bins wide, and the wide window is
  // 'fWideWindow' bins wide.

  if (Qhisto.size() && Nhisto.size()) {
    // First, find the peak time, using non-charge-weighted histogram.
    int peak = 0;
    for (unsigned int i = 0; i < Nhisto.size(); i++) {
      if (Nhisto[i] > peak) {
        time = i;
        peak = Nhisto[i];
      }
    }

    // If we are a low charge event, with not many QT pulses per bin,
    // look at the time instead
    if (peak <= fNQThresh) {
      int peak = 0;
      time = 0;
      for (unsigned int i = 0; i < Qhisto.size(); i++) {
        if (Qhisto[i] > peak) {
          time = i;
          peak = Qhisto[i];
        }
      }
    }

    // Now, calculate narrow and wide integrals.
    int start = time - fStartOffset;
    if (start < 0) {
      start = 0;
    }
    int endnarrow = start + fNarrowWindow;
    int endwide = start + fWideWindow;
    if (endnarrow > (int) Qhisto.size()) {
      endnarrow = Qhisto.size() - 1;
    }
    if (endwide > (int) Qhisto.size()) {
      endwide = Qhisto.size() - 1;
    }
    for (int i = 0; i < Qhisto.size(); i++) {
      if (i >= start && i < endwide) {
        wide += Qhisto[i];
      }
      if (i >= start && i < endnarrow) {
        narrow += Qhisto[i];
      }
      total += Qhisto[i];
    }

    // Check which Energy/Fprompt bin we're in. We don't explicitly calculate
    // an Fprompt as the the ODB settings don't deal with fractions.
    if (narrow < fLowE) {
      efpbin = BOX_VERYLOWE;
    } else if (narrow < fMedE) {
      if (256 * narrow > fFpromptLowE * wide) {
        efpbin = BOX_LOWE_HIGHFP;
      } else {
        efpbin = BOX_LOWE_LOWFP;
      }
    } else if (narrow < fHighE) {
      if (256 * narrow > fFpromptMedE * wide) {
        efpbin = BOX_MEDE_HIGHFP;
      } else {
        efpbin = BOX_MEDE_LOWFP;
      }
    } else {
      efpbin = BOX_HIGHE;
    }
  }

  // For now, we always save QT and ZLE information. Eventually, we will
  // decide based on which bin we're in.
  saveZLE = true;
  saveQT = true;

  // Create a summary bank, showing if we saved the ZLE and QT banks, and why.
  // Format (in bits) is:
  // VVVV00TTTTTTTTTTTTTTTTTTTTBBBBQZ
  // where V is bank version
  //       T is EB peak time (in 4ns bins)
  //       B is Energy/Fprompt box
  //       Q is whether QT data is saved
  //       Z is whether ZLE data is saved
  BYTE bank_version = 0x1; // Maximum value is 0xf!!!
  DWORD *pdata;
  bk_create(pevent, "EBSM", TID_DWORD, (void **) &pdata);
  DWORD word = 0;
  if (saveZLE)
    word |= 0x1;
  if (saveQT)
    word |= 0x2;
  word |= ((efpbin & 0xf) << 2); // 4 bits
  word |= (bank_version << 28);

  // Convert time to 4ns bins
  time *= fRebinFactor;
  word |= ((time & 0xfffff) << 6); // 20 bits

  *pdata++ = word;

  // We also save the calculated narrow/wide integrals and total charge
  // (32bits each).
  *pdata++ = narrow;
  *pdata++ = wide;
  *pdata++ = total;

  bk_close(pevent, pdata);

  return true;

	// Should be doing something with this....
	// Pass AccQhisto to FpFilter()
	/*	int FPBoxID = 0;;
	//	FPBoxID = FPFilter(AccQhisto);
	switch (FPBoxID) {
	case BOX1_LOW_E_HIGHFP:
		// set bit informing of what to do next
		break;
	case BOX2_HIGH_E_HIGHFP:
		// set bit informing of what to do next
		break;
	case BOX3_LOW_E_LOWFP:
		// set bit informing of what to do next
		break;
	case BOX4_HIGH_E_LOWFP:
		// set bit informing of what to do next
		break;
	case BOX5_HIGH_E:
		// set bit informing of what to do next
	default:
		// set bit informing of what to do next
		break;
		}*/

	/*
	 * Based on the FpFilter result and DTMTmsk content, compose the final event
	 * for example
	 * if DTM says VETO as well => add (VETO*)
	 * if DTM says Prescale => similar to BOX 1
	 * if DTM says Calibration => DTM+ZL+W4+(VETO*)
	 * BOX1: DTM+ZL+QT+W4+(VETO*)
	 * BOX2: DTM+ZL+QT+W4+(VETO*)
	 * BOX3: DTM+QT+(VETO*)
	 * BOX4: DTM+QT+W4+(VETO*)
	 * BOX5: DTM+ZL+QT+W4+(VETO*)
	 */

}
