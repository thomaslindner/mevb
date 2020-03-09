#ifndef EBFilterDecision_hxx
#define EBFilterDecision_hxx

#include <vector>
#include "midas.h"

/**
 * This class is meant to encapsulate the event builder level Filter decision.
 *
 * The class has one main method 'FPromptDecision' which will take as input a 
 * Q vs T summary histogram from ZLE data and decide which parts of the event
 * to save.  
 */
class ebFilterDecision {
  public:
    ebFilterDecision();
    virtual ~ebFilterDecision() {}

    // Main method; take QvsT input and makes filter decision.
    bool FPromptDecision(std::vector<int>& Qhisto, std::vector<int>& Nhisto,
        char *pevent, bool &saveZLE, bool &saveQT);

    // Read settings for the filter decision from the ODB.
    // hDB and hsf must already be setup to read from the EBuilder settings!
    void ReadODB(HNDLE& hDB, HNDLE& hsf);

    enum FPBoxID {
      BOX_NOTSET = 0,
      BOX_VERYLOWE,
      BOX_LOWE_LOWFP,
      BOX_LOWE_HIGHFP,
      BOX_MEDE_LOWFP,
      BOX_MEDE_HIGHFP,
      BOX_HIGHE
    };

  private:
    int fRebinFactor; // Number of 4ns bins to combine into one for the summary histogram
    int fLowE;        // Lower bound of Low E box (in ADC)
    int fMedE;        // Low/medium E boundary (in ADC)
    int fHighE;       // Medium/high E boundary (in ADC)
    int fFpromptLowE; // Fprompt split for low E (in units of 1/256)
    int fFpromptMedE; // Fprompt split for medium E (in units of 1/256)
    int fStartOffset; // How long before the peak to start the integral windows (in bins)
    int fNarrowWindow;// Narrow integral width (in bins)
    int fWideWindow;  // Wide integral width (in bins)
    int fNQThresh;    // When to change the logic of using Nhisto vs Qhisto to find peak.
};

#endif

