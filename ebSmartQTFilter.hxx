#ifndef ebSmartQTFilter_hxx
#define ebSmartQTFilter_hxx

#include <vector>
#include <map>
#include <set>
#include "midas.h"

/**
 * This class is meant to encapsulate the event builder level Filter decision.
 *
 * The class has one main method 'FPromptDecision' which will take as input a 
 * Q vs T summary histogram from ZLE data and decide which parts of the event
 * to save.  
 */
class ebSmartQTFilter {
  public:
    ebSmartQTFilter();
    virtual ~ebSmartQTFilter() {}

    // Read settings for the filter from ODB. Returns whether we were
    // successful or not.
    bool ReadODB(HNDLE& hDB);

    void AnalyzeBanks(char * src);
    void WriteFilteredBanks(char * dest_pevent, char * src);
    void ResetV1720Analysis();
    void ResetV1740Analysis();

    void SetSaveAllQT(bool set) { fSaveAllQT = set; }
    void SetSaveAllSmartQT(bool set) { fSaveAllSmartQT = set; }

    bool GetSaveAllQT() { return fSaveAllQT; }
    bool GetSaveAllSmartQT() { return fSaveAllSmartQT; }

  private:

    // Return whether we should keep the ZLE for this block, based on the
    // analysis results. AnalyzeBanks() must have been previously called.
    bool ShouldKeepZLE(int module, int channel, int offset);

    // Return whether we should keep the V1740 for this group, based on the
    // analysis results. AnalyzeBanks() must have been previously called.
    // Note that each group is of 8 channels!
    bool ShouldKeepV1740(int board, int group);

    // Return whether any channels have been flagged for saving V1740 information.
    // AnalyzeBanks() must have been previously called.
    bool ShouldKeepAnyV1740();

    bool fSaveAllQT; // Override that means we we'll save all the QT for this event.
    bool fSaveAllSmartQT; // Override that means we we'll save all the Smart QT for this event.

    bool fDebugKeepZLECopy;
    bool fDebugKeepSQCopy;
    bool fDebugKeepW4Copy;
    bool fDebugKeepMN;

    bool fSaveSmartQTEvenIfSavingZLE;

    bool fEventCount;

    // Save if SPE confidence > threshold
    INT fV1720SPEConfidenceThreshold;

    // Whether to filter V1720 filtering based on Smart QT results
    BOOL fEnableV1720Filtering;

    // Whether to enable the filtering of V1740 waveforms based on V1720 waveform heights
    BOOL fEnableV1740Filtering;

    // If V1720 minimum is below this value, we'll save V1740 information too
    INT fSaveV1740Threshold;

    // Map from V1720 index (module * 8 + channel) to V1740 index (module * 64 + channel)
    INT fV1720V1740Map[256];


    // `module * 8 + channel` -> block offsets to filter ZLE
    std::vector<std::vector<int> > fAnalysisResultsFilterZLE;

    // Set of `board * 8 + channel` that V1740 should be saved for
    std::set<int> fAnalysisResultsSaveV1740;
};

#endif

