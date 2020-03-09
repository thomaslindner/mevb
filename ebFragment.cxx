/*****************************************************************************/
/**
\file ebFragment.cxx
 
\section contents Contents
Deap Event Builder

\subsection notes Notes about this class
N/A

\subsection usage Usage
N/A

\section deap DEAP-3600 notes
none for now
 *****************************************************************************/

#include "ebFragment.hxx"
#include <execinfo.h>

#define UNUSED(x) ((void)(x)) //!< Suppress compiler warnings

//! Configuration string for this Buffer. (ODB: /Equipment/[eq_name]/Settings/[buffername]/)
const char * EBFragment::config_str_fragment[] = {\
    "Buffer Name = STRING : [32] BUF01",\
    "Event ID = WORD : -1",\
    "Trigger Mask = WORD : -1",\
    "Enable = BOOL : y",\
    NULL
};

const char EBFragment::history_settings[][NAME_LENGTH] = { "rb_level" };
//---------------------------------------------------------------------------------
/**
 * \brief   Constructor for the module object
 *
 * Set the basic hardware parameters
 *
 * \param   [in]  evid      Event ID for that buffer
 * \param   [in]  tmsk      Trigger Mask for that Buffer
 */
EBFragment::EBFragment(HNDLE hDB)
: evid_(0), tmsk_(-1), odb_handle_(hDB), num_events_in_rb_(0)
{
	dtmtmsk_ = -1;
  eqp_name_ = "";
  buffer_name_ = "";
  fe_name_ = "";
  enable_ = true;
  buffer_handle_ = -1;
  setting_frag_handle_ = 0;
  settings_loaded_ = false;
  settings_touched_ = false;
  running_= false;
  rb_handle_ = -1;
  requestID_=-1;
  fragmentID_=-1;
  verbosity_ = 0;
	thread_status_ = 0;
	fTimeStampErrors = 0;
	fRebinFactor = 0;
}

//---------------------------------------------------------------------------------
/**
 * Move constructor needed because we're putting eBuilder objects in a vector which requires
 * either the copy or move operation.  The implicit move constructor (or copy constructor)
 * cannot be created by the compiler because our class contains an atomic object with a
 * deleted copy constructor. */
EBFragment::EBFragment(EBFragment&& other) noexcept
: evid_(std::move(other.evid_)), tmsk_(std::move(other.tmsk_)), enable_(std::move(other.enable_))
  	  , odb_handle_(std::move(other.odb_handle_))
  	  , num_events_in_rb_(other.num_events_in_rb_.load())
{
	dtmtmsk_ = std::move(other.dtmtmsk_);
  buffer_name_ = std::move(other.buffer_name_);
  fe_name_ = std::move(other.fe_name_);
  eqp_name_ = std::move(other.eqp_name_);
  buffer_handle_ = std::move(other.buffer_handle_);
  setting_frag_handle_ = std::move(other.setting_frag_handle_);
  settings_loaded_ = std::move(other.settings_loaded_);
  settings_touched_ = std::move(other.settings_touched_);
  running_= std::move(other.running_);
  rb_handle_ = std::move(other.rb_handle_);
  verbosity_ = std::move(other.verbosity_);
	thread_status_= std::move(other.thread_status_);
  requestID_ = std::move(other.requestID_);
  fragmentID_ = std::move(other.fragmentID_);
  config = std::move(other.config);
	fTimeStampErrors = std::move(other.fTimeStampErrors);
	fRebinFactor = std::move(other.fRebinFactor);
}

//---------------------------------------------------------------------------------
EBFragment& EBFragment::operator=(EBFragment&& other) noexcept
{
  if (this != &other){  //if trying to assign object to itself

    buffer_name_ = std::move(other.buffer_name_);
    fe_name_ = std::move(other.fe_name_);
    eqp_name_ = std::move(other.eqp_name_);
    evid_ = std::move(other.evid_);
    tmsk_ = std::move(other.tmsk_);
    enable_ = std::move(other.enable_);
    odb_handle_ = std::move(other.odb_handle_);
    num_events_in_rb_ = other.num_events_in_rb_.load();
    buffer_handle_ = std::move(other.buffer_handle_);
    setting_frag_handle_ = std::move(other.setting_frag_handle_);
    settings_loaded_ = std::move(other.settings_loaded_);
    settings_touched_ = std::move(other.settings_touched_);
    running_= std::move(other.running_);
    rb_handle_ = std::move(other.rb_handle_);
    verbosity_ = std::move(other.verbosity_);
    thread_status_ = std::move(other.thread_status_);
    config = std::move(other.config);
		fTimeStampErrors = std::move(other.fTimeStampErrors);
	  fRebinFactor = std::move(other.fRebinFactor);
  }
  return *this;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Destructor for the module object
 *
 * Nothing to do.
 */
EBFragment::~EBFragment()
{
}

//---------------------------------------------------------------------------------
/**
 * \brief   Sorting function based on trigger mask
 *
 * \return
 */
bool EBFragment::operator <(const EBFragment& rhs) const
 {
    return (this->tmsk_ < rhs.tmsk_);
 }


//---------------------------------------------------------------------------------
/**
 * \brief   Get short string identifying the module's index, link and Buffer number
 *
 * \return  name string
 */
std::string EBFragment::GetName()
{
  std::stringstream txt;
  txt << "B " << std::setfill('0') << std::setw(2) << buffer_name_
      << " ID " << std::setfill('0') << std::setw(2) << evid_
      << " MSK " << std::setfill('0') << std::setw(2) << tmsk_;
  return txt.str();
}


//---------------------------------------------------------------------------------
/**
 * \brief   Get run status
 *
 * \return  true if fragment is enabled
 */
bool EBFragment::IsEnabled()
{
  return enable_;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Get run status
 *
 * \return  true if run is started
 */
bool EBFragment::IsRunning()
{
  return running_;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Disconnect the Buffer through the optical link
 *
 * \return  yes if it is
 */
bool EBFragment::Disconnect()
{
  if (verbosity_) std::cout << GetName() << "::Disconnect()\n";

  if (IsRunning()) {
    cm_msg(MERROR,"Disconnect","Can't disconnect Buffer %s: run in progress", this->buffer_name_.c_str());
    return false;
  }

  return true;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Get BM Buffer level in bytes
 *
 * Return the current BM buffer level for that fragment object
 *
 * \param   [out]  val     Number of events stored
 * \return  [out] level    in Bytes
 */
int EBFragment::GetBMBufferLevel(int hBuf)
{
	int level;
	bm_get_buffer_level(hBuf, &level);
	printf("Buffer %d, level %d, info: \n", hBuf, level);
	return level;
}

#define TS2_IDX             1    //!< Time Stamp V1720 copy to QT bank
#define N_DWORD_IDX         2    //!< Number of 32-bit words from the QT bank
#define QCH_IDX             0    //!< Channel/bases index        (triplet)
#define BASE_IDX            1    //!< 1st, basebefore, baseafter (triplet)
#define QINTEGRAL_IDX       2    //!< Integral                   (triplet)
#define TS_IDX              3    //!< Time Stamp from the V1720/V1740/Veto bank
static const int gTimeStampMask = 0x3fffffff;


//---------------------------------------------------------------------------------
/**
 * \brief   Read event fragment from this buffer, place it at wp in the rb
 *
 * Read one event from this Midas buffer and place it in the ringBuffer.
 * 
 * Also, loop over the actual banks and come up with a summed QvsT histogram.
 * Also, save the earliest and latest timestamps for this fragment.
 *
 * \param   [in]  wp     Write pointer to the ring buffer
 *                       pointer will be incremented before exit of this func()
 * \return  function success
 */
extern INT max_event_size;
bool EBFragment::ReadFragment(void *wp)
{
	char *pdata = (char *)wp;
	int status, size;
	int diff;
	
	size = max_event_size;
	status = bm_receive_event(this->buffer_handle_, pdata, &size, BM_NO_WAIT);
	switch (status) {
	case BM_SUCCESS:      /* event received */
		break;
	case BM_ASYNC_RETURN: /* timeout */

	 
		// Do timeout handling

		// Do a check of when we last received an event...
		// Print error if it seems like we haven't gotten an event in a while...
		diff = ss_time() -fLastTimeReadEvent;
			if(fLastTimeReadEvent != 0 && diff > 40 && !fLastTimeReadEventWarn){
			cm_msg(MERROR,"ReadFragment", "Haven't seen a new event from fragment %s (ID=%d) for more than 40 seconds.", this->GetEqpName().c_str(), this->GetFragmentID());
			fLastTimeReadEventWarn = true;
		}

		if(fLastTimeReadEvent != 0 && diff > 50 && !fLastTimeReadEventError){
			cm_msg(MERROR,"ReadFragment", "Haven't seen a new event from fragment %s (ID=%d) for more than 50 seconds.  Front-end probably died; event builder will freeze; run is probably dead.", this->GetEqpName().c_str(), this->GetFragmentID());
			fLastTimeReadEventError = true;
		}

		// If we haven't read an event yet, then set the LastTimeReadEvent to current time.
		// This allows us to catch cases where a front-end never produces fragments.
		if(fLastTimeReadEvent == 0)  fLastTimeReadEvent = ss_time();


		return false;
		break;
	default:              /* Error */
		cm_msg(MERROR, "source_scan", "bm_receive_event error %d", status);
		return false;
		break;
	}

	// Remember the last time we got event...
	fLastTimeReadEvent = ss_time();

	/* Loop over all the banks
	 * QT banks have a V1720 TS copy, for the ZL or ?W2? banks.
	 * For the other fragment, retrieve the TS from the banks themselves (W4, VE)
	 * Banks W4, VE are mutually exclusive!
	 */
	// Reset the rb area containing the min/maxTS and the 250 bins of Q-histo
	BANK32 *pbh = NULL;
	DWORD *pdata_b = NULL;
  // char *pdata;
	INT bksize;
	EVENT_HEADER *pevent;
	
	// Top of the fragment event
	pevent = (EVENT_HEADER *)pdata;

	// Min/Max TimeStamp extraction
	DWORD tsmax = 0, tsmin = 0xFFFFFFFF;
	// Also, store the timestamp for the first module of each group.
	DWORD first_module_timestamp = 0;
	int nbank = 0;

	// Figure out if this event has V1720 data (specifically QT banks).
	bool bV1720 = false;

	/// Use size from event header, instead of from bm_receive_event; seems more reliable.
	int event_size = ((EVENT_HEADER *) pdata)->data_size + sizeof(EVENT_HEADER);

	// pointer to qvst histogram
	// (after the 2 DWORD for Tmin/Tmax and 1 DWORD for number time bins) and 1 for control word
	DWORD *qt_list = (DWORD*)pdata + event_size/sizeof(DWORD);
	DWORD *qhisto = (DWORD*)qt_list + 4;
	//DWORD *qhisto = (DWORD*)wp + 4;
 
	// Create vectors for saving QT summary histograms.
	std::vector<int> qvect;
	std::vector<int> nvect;

	// Iterate through all the QT banks found in this fragment
	if(1)
		do {
		bksize = bk_iterate32((BANK_HEADER *) (pevent + 1), &pbh, &pdata_b);
		//std::cout << "Bank name " << bksize << std::endl;
		if(bksize){
      if (strncmp(pbh->name, "ZL", 2) == 0) { // Handle ZL banks
        // Get the module number
        std::string sname(pbh->name);
        std::string smodule = sname.substr(2, 2);
        int module = atoi(smodule.c_str());
        // Save timestamp if this is first module in a group.
        if((module == 0 && GetTmask() == 0x2) ||
           (module == 8 && GetTmask() == 0x4) ||
           (module == 16 && GetTmask() == 0x8) ||
           (module == 24 && GetTmask() == 0x10)){
          first_module_timestamp = pdata_b[TS_IDX];
        }
      }
			if (strncmp(pbh->name, "QT", 2) == 0) { // Handle QT banks
				// Get the module number
				std::string sname(pbh->name);
				std::string smodule = sname.substr(2, 2);
				int module = atoi(smodule.c_str());
				// Save timestamp if this is first module in a group.
				if((module == 0 && GetTmask() == 0x2) || 
					 (module == 8 && GetTmask() == 0x4) ||
					 (module == 16 && GetTmask() == 0x8) ||
					 (module == 24 && GetTmask() == 0x10)){
					first_module_timestamp = pdata_b[TS2_IDX];
				}

				int ndwords = pdata_b[N_DWORD_IDX];
				// Save timestamps.
				if(pdata_b[TS2_IDX] > tsmax) tsmax = pdata_b[TS2_IDX];
				if(pdata_b[TS2_IDX] < tsmin) tsmin = pdata_b[TS2_IDX];

				// Loop over qt values.
				for (int i = QINTEGRAL_IDX+1 ; i < QINTEGRAL_IDX+ndwords+1; i+=4) {

					int min_bin = ((pdata_b[i+3] >> 16) & 0xFFFF);
					int integral = (pdata_b[i+2] & 0xFFFFFF);

					// Rebin the time base.
					int bin = min_bin / fRebinFactor;

					// Ensure the vectors are long enough
					if (bin >= (int)qvect.size()) {
            qvect.resize(bin+1, 0);
            nvect.resize(bin+1, 0);
					}

					// Get the original value for this bin
					unsigned long int charge = qvect[bin];
					if(charge > 4000000000 - integral) {
					  charge = 4000000000;
					} else {
					  charge += (unsigned long int)integral;
					}
					qvect[bin] = charge;
					nvect[bin]++;
				}
				nbank++;
				bV1720 = true;
			}
			if (strncmp(pbh->name, "W2", 2) == 0) { // Handle W2XX raw V1720 banks
				// Get the module number
				std::string sname(pbh->name);
				std::string smodule = sname.substr(2, 2);
				int module = atoi(smodule.c_str());
				// Save timestamp if this is first module in a group.
				if((module == 0 && GetTmask() == 0x2) || 
					 (module == 8 && GetTmask() == 0x4) ||
					 (module == 16 && GetTmask() == 0x8) ||
					 (module == 24 && GetTmask() == 0x10)){
					first_module_timestamp = pdata_b[TS_IDX];
				}
			}
			if (strncmp(pbh->name, "W4", 2) == 0) {
				if(pdata_b[TS_IDX] > tsmax) tsmax = pdata_b[TS_IDX];
				if(pdata_b[TS_IDX] < tsmin) tsmin = pdata_b[TS_IDX];
				nbank++;
				std::string sname(pbh->name);
				std::string smodule = sname.substr(2, 2);
				int module = atoi(smodule.c_str());
				if(module == 0)
					 first_module_timestamp = pdata_b[TS_IDX];

			}
			if (strncmp(pbh->name, "VETO", 4) == 0) {
				if(pdata_b[TS_IDX] > tsmax) tsmax = pdata_b[TS_IDX];
				if(pdata_b[TS_IDX] < tsmin) tsmin = pdata_b[TS_IDX];
				nbank++;
			}
			if (strncmp(pbh->name, "CALI", 4) == 0) {
				if(pdata_b[TS_IDX] > tsmax) tsmax = pdata_b[TS_IDX];
				if(pdata_b[TS_IDX] < tsmin) tsmin = pdata_b[TS_IDX];
				nbank++;
				bV1720 = true;
			}
		}
		} while (bksize);
	
	// Convert the Q and N vectors into arrays for saving in the bank.
	// Q and N are both the same size, and 1 DWORD is 4 bytes.
	// We store the two arrays consecutively.
	int noffset = qvect.size();
	int nqtbins = qvect.size() * 2;
  memset ((char *) qhisto, 0, 4*nqtbins);
  for (unsigned int i = 0; i < qvect.size(); i++) {
    qhisto[i] = qvect[i];
    qhisto[i + noffset] = nvect[i];
  }

	// If we have a timestamp from the first module, then save it.
	// Otherwise use the lowest timestamp.
	DWORD best_timestamp;
	if(first_module_timestamp != 0){
		best_timestamp = first_module_timestamp;
	}else{
		best_timestamp = tsmin;
	}

	// Store the TimeStamp min/max in first wp location.
	// Need a convention.  Let's say that we are saving 
	// the 16ns counter values, for 30 bits.
	if (bV1720){ // V1720 seems to be 8ns counter; down-convert.

		qt_list[0]     = (DWORD(best_timestamp >>1) & gTimeStampMask);
		qt_list[1] = (DWORD(tsmax >>1) & gTimeStampMask);
	}else{//V1740 counters seem to be 32 ns counter.
		qt_list[0]     = ((best_timestamp >> 1) & gTimeStampMask);
		qt_list[1] = ((tsmax >> 1) & gTimeStampMask);
	}

	// Store control word
	qt_list[3] = 0xdeadbeef;

			
	// Increment wp pointer to next available location
	if (bV1720){		
		qt_list[2] = nqtbins;
		event_size += (4+nqtbins)*sizeof(DWORD);
		rb_increment_wp(this->GetRingBufferHandle(), event_size);
	}	else {		
		qt_list[2] = 0;
		event_size += 4*sizeof(DWORD);
		rb_increment_wp(this->GetRingBufferHandle(), event_size);
	}
	
	//this->IncrementNumEventsInRB(); //atomic
	return true;
}

//---------------------------------------------------------------------------------
//---------------------------------------------------------------------------------
/**
 * \brief   Print the Bank list of this fragment (pev)
 *          No addition to the rb
 *
 * \param   [in]  pev    (Read) pointer to the current fragment
 * \param   [in]  wp     valid Write pointer at the end of the current fragment
 *                       wp will be incremented before exit of this func()
 * \return       number of banks found in that func()
 */
int EBFragment::BankListOfFragment(void *pev)
{
	INT status;
	EVENT_HEADER *pevent;
	char sbanklist[256];

	// Top of the fragment event
	pevent = (EVENT_HEADER *)pev;

	status = bk_list((BANK_HEADER *) (pevent + 1), sbanklist);
	printf("\n#banks:%i - Bank list:-%s-\n", status, sbanklist);

	return status;
}

//---------------------------------------------------------------------------------
//---------------------------------------------------------------------------------
/**
 * \brief   Extract the Midas Serial Number from the fragment event
 *
 * \param   [in]  pev    (Read) pointer to the current fragment
 * \param   [in]  wp     valid Write pointer at the end of the current fragment
 *                       wp will be incremented before exit of this func()
 * \return       number of banks found in that func()
 */
DWORD EBFragment::GetSNFragment(void)
{
	char * src;
	// the src is in the rb and contains a full Midas event
	int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&src, 5000);
	if (status == DB_TIMEOUT) {
		cm_msg(MERROR,"GetSNFragment", "Got rp timeout for fragmentID %s (%d)", this->GetEqpName().c_str(), this->GetFragmentID());
		exit(0);
		return false;
	}

	return ((EVENT_HEADER*)src)->serial_number;
}



	// Method to check if the control word in the ring buffer is correctly set.
	// rbp points to the start of this ring buffer event.
//---------------------------------------------------------------------------------
/**
 * \brief Method to check if the control word in the ring buffer is correctly set.
 *
 * Method to check if the control word in the ring buffer is correctly set.
 *
 * \param   [char*]  rbp points to the start of this ring buffer event.
 * \return  [bool] true if the control sample matchs.
 */
bool EBFragment::CheckControlWord(char *rbp){

	DWORD tevsize = ((EVENT_HEADER *)rbp)->data_size + sizeof(EVENT_HEADER);// + 4;
	DWORD *qt_list = (DWORD*)rbp + tevsize/sizeof(DWORD);
	unsigned int control = qt_list[3]; // control word, should be deadbeef
	if(control != 0xdeadbeef){
		std::cout << "Fail! " << std::endl;
		unsigned int initial = control;
		bool success = false;
		int i;
		for(i = 0; i < 1000000; i++){
			usleep(1);
			control = qt_list[3];
			if(control == 0xdeadbeef){
				success = true;
				break;
			}
		}
		
		if(success){
			if(i >= 0){
				std::cout << "\nControl word not correct; delay. " << initial << std::endl;
				std::cout << "Got the right control word after " << (i+1) << " us." << std::endl;
				std::cout << "Number events in ring buffer: " << this->GetNumEventsInRB() << std::endl;
				return true;
			}
			return true;
		}else{
			cm_msg(MERROR,"EBFragment::CheckControlWord", "Control word not correct after 10 seconds"); 
			return false;
		}
	}
	return true;
}


//---------------------------------------------------------------------------------
/**
 * \brief   Get the DTM TimeStamp reference and the Trigger mask
 *
 * Extract from the DTM fragment, the TS and Tmsk
 * Bank name is fixed "DTM_"
 *
 * \param   [in]  pev       (Read) pointer to the current fragment
 * \param   [in]  dtmts     Reference Time Stamp 
 * \param   [in]  dtmtmask  DTM trigger mask requested for this event
 * \return       >0 if ok
 */
bool EBFragment::GetDTMTsTmsk(void ** pev, DWORD * dtmts, WORD * dtmtmask)
{
	// Check for missing event and disable fragment
	if (! this->IsEnabled()) {
		if (this->tmsk_ != 0xFFFF) {
			cm_msg(MERROR, "readout", "Main trigger event not enabled!");
		}
		assert(0);   // No go
	}

	// Get rb event point to top event
	int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&pev, 100);
	if (status == DB_TIMEOUT) {
		cm_msg(MERROR,"GetDTMTsTmsk", "Got rp timeout for fragmentID %s (%d)", this->GetEqpName().c_str(),  this->GetFragmentID());
		printf("### num events: %d\n", this->GetNumEventsInRB());
		pev = NULL;
		return 0;
	}

	EVENT_HEADER *pevent;
	DWORD *pdata = NULL;
	pevent = (EVENT_HEADER *) &pev;

	// locate DTM bank
	bk_locate(pevent, "DTM_", pdata);
	if (pdata == NULL) {
		printf("Cannot find DTM_ bank -> abort!\n");
		assert(pdata);
	}
    // Extract trigger mask
    // Todo: Check indices
	*dtmtmask = pdata[3];
	* dtmts   = pdata[4];
    return 1;
}

//---------------------------------------------------------------------------------
/**
 * \brief   extract the fragment event from the ring buffer
 *
 * 1) Extract the TimeStamp from the "Extra" area and compare it to the DTMTref
 * 2) Extract the cummulated the qhisto from the "Extra" area
 *
 * \param   [in]      fragment idx in the Vector
 * \param   [in/out]  QvsT histo to add fragment pulse to.
 * \param   [in/out]  Npulses vs T histo to add fragment pulse to.
 * \param   [in]      TimeStamp reference from the DTM for TS comparaison
 *
 * \return  true if ok, false if the timestamps don't match.
 */
bool EBFragment::CheckTSAndGetQT(int idx, std::vector<int>& Qhisto, std::vector<int>& Nhisto, DWORD ts)
{
	// Check for missing event and disable fragment
	if (! this->IsEnabled()) {
		if (this->tmsk_ != 0xFFFF) {
			cm_msg(MERROR, "readout", "Main trigger event not enabled!");
		}
		assert(0);   // No go
	}

	char * pevent;
	// Get rb event pointer to top event
	int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&pevent, 100);
	if (status == DB_TIMEOUT) {
		cm_msg(MERROR,"CheckTSAndGetQT", "Got rp timeout for module %s %d", 
					 this->GetName().c_str(),this->GetFragmentID());
		printf("### num events: %d\n", this->GetNumEventsInRB());
		pevent = NULL;
		return false;
	}
	
	// Check that control word looks ok; currently just error message if bad.
	bool controlCheck = CheckControlWord(pevent); 

	DWORD tevsize = ((EVENT_HEADER *)pevent)->data_size + sizeof(EVENT_HEADER);// + 4;
	DWORD *qt_list = (DWORD*)pevent + tevsize/sizeof(DWORD);

	unsigned int tmin = *(qt_list);
	unsigned int tmax = *(qt_list+1); 

	// First, take the 16ns timestamp from DTM (lose top two bit)
	//unsigned int convert_dtm = (ts & 0x3fffffff) * 2;
	unsigned int convert_dtm = (ts & gTimeStampMask);

	// Get the timestamp for this fragment.  Should be already converted to 16ns counter and correctly masked.
	unsigned int convert_v1720 = tmin;
	
	

	// Current difference (handle the roll-over cleanly)
	unsigned int currentTimeDiff = ((convert_v1720 - convert_dtm) & gTimeStampMask);



	// If this is the first event in this run, then grab the earliest time and use it to 
	// calculate the time difference from the DTM timestamp.
	if(!fTimeStampDiffSet){
		fTimeStampDifference = currentTimeDiff;
		fTimeStampDiffSet = true;
	}
	

	// Compare this timestamp difference to the timestamp difference for the first event.  Should be 
	// close enough together.  Otherwise print errors (and stop run?)
	int bitdiff = currentTimeDiff - fTimeStampDifference;
	int bitdiff2 = 1 + gTimeStampMask - bitdiff;
	int max_diff = 2;

	// status to return; true if timestamps match.
	bool timeStampCheckSuccess = true;

	if(abs(bitdiff) > max_diff 
		 && abs(bitdiff2) > max_diff){//This works, but it doesn't seem like optimal solution.  To revisit.   
		double diff = bitdiff*16.0/1e9 ;

		timeStampCheckSuccess = false;
		
		fTimeStampErrors++;
		if(fTimeStampErrors < 5 || fTimeStampErrors%50000==0 ){
			cm_msg(MERROR,"CheckTSAndGetQT", "Timestamp matching failure for fragment %s: time difference between this fragment and DTM fragment is %i, whereas we expected difference to be %i (16ns counter). This is %f sec (%i/%i counts) different from expected. Raw timestamp = %i. Number timestamp errors for this fragment = %i. Debug data: DTM TS=%i, convert_dtm=%i, convert_v1720=%i, currentTimeDiff=%i, bitdiff=%d, bitdiff2=%d, max_diff=%d", 
						 this->GetEqpName().c_str(),currentTimeDiff,fTimeStampDifference,diff,bitdiff,bitdiff2,
						 convert_v1720,fTimeStampErrors, ts, convert_dtm, convert_v1720, currentTimeDiff, bitdiff, bitdiff2, max_diff);
			//if(GetFragmentID()  == 1){
			//exit(0);
			//}
			
		}
	}

	// Update the cumulative Q vs T histogram using results from this ring buffer.
	// We have a vector of QvsT followed by NvsT, so divide the saved number in 2
	// to get the length of each vector.
	int nQTBins = qt_list[2] >> 1;
  Qhisto.resize(nQTBins, 0);
  Nhisto.resize(nQTBins, 0);

	// If no QvsT information, just return.
	if(nQTBins == 0)
		return timeStampCheckSuccess;
	
	int offset = 4;
	for(int i = 0; i < nQTBins; i++){
    Qhisto[i] += qt_list[offset + i];
    Nhisto[i] += qt_list[nQTBins + offset + i];
	}
	
	return timeStampCheckSuccess;

}

//---------------------------------------------------------------------------------
/**
 * \brief   Compose the final event with all the fragments from the Rbs
 *
 * \param   [in/out]  Final event pointer
 *
 * \return  true if ok 
 */
bool EBFragment::AddBanksToEvent(char * pevent)
{
  
  
  /*
   * pevent: points after the EVENT_HEADER (header alread composed in mfe
   * and initialized by the caller (bk_init32(pevent))
   *
   * src   : points at the EVENT_HEADER (full event)
   *         event size = pevent->data_size + sizeof(EVENT_HEADER)
   */
  char *src;
  // the src is in the rb and contains a full Midas event
  int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&src, 1000);
  if (status == DB_TIMEOUT) {
    cm_msg(MERROR,"AddBanksToEvent", "Got rp timeout for fragmentID %s %d", this->GetName().c_str(),this->GetFragmentID());
    printf("### num events: %d\n", this->GetNumEventsInRB());
    return false;
  }
  
  // Check that control word looks ok; currently just error message if bad.
  bool controlCheck = CheckControlWord(src);
  
  
  // Move Read pointer to next fragment
  DWORD tevsize = ((EVENT_HEADER *)src)->data_size + sizeof(EVENT_HEADER);// + 4;
  DWORD *qt_list = (DWORD*)src + tevsize/sizeof(DWORD);
  int nQTBins = qt_list[2];
  tevsize += (4+nQTBins)*sizeof(DWORD);
  
  rb_increment_rp(this->GetRingBufferHandle(), tevsize );
  
  // Inform main thread of new fragment
  this->DecrementNumEventsInRB(); //atomic
  
  return true;
}

//---------------------------------------------------------------------------------
bool EBFragment::FillStatBank(char * pevent, suseconds_t usStart)
{
  return false;
}

//---------------------------------------------------------------------------------
bool EBFragment::FillBufferLevelBank(char * pevent)
{


  return false;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Set the ODB record for this Buffer
 *
 * Create a record for the Buffer with settings from the configuration
 * string (eBuilder::config_str_Buffer) if it doesn't exist or merge with
 * existing record. Get the handle to the record.
 *
 * \param   [in]  h        main ODB handle
 * \return  ODB Error Code (see midas.h)
 */
int EBFragment::SetFragmentRecord(HNDLE h )
{
  char set_str[200];
  int status,size;

  sprintf(set_str, "/Equipment/EBuilder/Settings/%s", this->eqp_name_.c_str());

  if (this->verbosity_)  std::cout << GetEqpName() << "::SetFragmentRecord(" << h << "," << set_str << ",...)" << std::endl;

  //create record if doesn't exist and find key
  status = db_create_record(h, 0, set_str, strcomb(config_str_fragment));
  status = db_find_key(h, 0, set_str, &(this->setting_frag_handle_));
  if (status != DB_SUCCESS) {
    cm_msg(MINFO,"SetFragmentRecord","Key %s not found. Return code: %d", set_str, status);
  }
  size = sizeof(config);
	status = db_get_record(h, this->setting_frag_handle_, &config, &size, 0);
	if (status != DB_SUCCESS){
		cm_msg(MERROR,"GetFragmentRecord","Couldn't get record %s. Return code: %d", set_str, status);
		return status;
  }

  // Get current record for the Enable setting (if exists)
  size = sizeof(config);
  status = db_get_record(h, this->setting_frag_handle_, &config, &size, 0);
  if (status != DB_SUCCESS){
    cm_msg(MERROR,"GetFragmentRecord","Couldn't get record %s. Return code: %d", set_str, status);
    return status;
  }

  // Write the object in the newly ODB tree
  sprintf(config.buffername, "%s", this->buffer_name_.c_str());
	//   config.enable     = this->enable_; // Don't update the enable; that should be set from web
  config.evid       = this->evid_;
  config.tmsk       = this->tmsk_;

  // Set actual record
  size = sizeof(config);
  status = db_set_record(h, this->setting_frag_handle_, &config, size, 0);
  if (status != DB_SUCCESS){
    cm_msg(MERROR,"SetFragmentRecord","Couldn't set record %s. Return code: %d", set_str, status);
    return status;
  }

  settings_loaded_ = true;
  settings_touched_ = true;

  return status; //== DB_SUCCESS for success
}

//---------------------------------------------------------------------------------
/**
 * \brief   Initialize the hardware for data acquisition
 *
 * ### Initial setup:
 * ### Checks
 * \return  0 on success, -1 on error
 */
INT EBFragment::InitializeForAcq()
{
  if (verbosity_) std::cout << GetName() << "::InitializeForAcq()" << std::endl;

  if (!settings_loaded_) {
    cm_msg(MERROR,"InitializeForAcq","Cannot call InitializeForAcq() without settings loaded properly on Buffer %s", this->GetBufferName().c_str());
    return -1;
  }

  if (IsRunning()){
    cm_msg(MERROR,"InitializeForAcq","Buffer %s already started", this->GetBufferName().c_str());
    return -1;
  }

  //don't do anything if settings haven't been changed
  if (settings_loaded_ && !settings_touched_)
      return 0;
  return 0;
}

std::pair<unsigned int,unsigned int> EBFragment::GetDTMTriggerMaskUsed(){
	
	// Make sure this is DTM fragment
	if(GetTmask() != 0x1)	return std::pair<unsigned int,unsigned int>(-1,-1) ;
	
	// Check if we have DTM fragment in ring buffer; if not, keep looping.
	if(GetNumEventsInRB() == 0) return std::pair<unsigned int,unsigned int>(-1,-1);
	
	// Now grab the event from the ring
	char * src;
	// the src is in the rb and contains a full Midas event
	int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&src, 1000);
	if (status == DB_TIMEOUT) {
		cm_msg(MERROR,"GetDTMTriggerMaskUsed", "Got rp timeout for fragmentID %s %d",this->GetName().c_str(), this->GetFragmentID());
		return std::pair<unsigned int,unsigned int>(-1,-1);
	}
	
	EVENT_HEADER *pevent;
	pevent = (EVENT_HEADER *)src;
	
	DWORD tevsize = ((EVENT_HEADER *)src)->data_size + sizeof(EVENT_HEADER);// + 4;

	DWORD *qt_list = (DWORD*)src + tevsize/sizeof(DWORD);



	unsigned int control = qt_list[3]; // control word, should be deadbeef
	if(control != 0xdeadbeef){
		std::cout << "GetDTMTriggerMaskUsed control fail! " << std::endl;
		for(int i = -4; i < 10; i++){
			std::cout << std::hex << qt_list[i] << std::dec << std::endl;
		}

	}

	int bksize;
	BANK32 *pbh = NULL;
	DWORD *pdata = NULL;
	
	// Loop over event, looking for DTRG bank...		
	do {
		//bksize = bk_iterate32(pevent, &pbh, &pdata);
		bksize = bk_iterate32((BANK_HEADER *) (pevent + 1), &pbh, &pdata);
		//std::cout << "Bank name " << bksize << std::endl;
		if(bksize){
			//				std::cout << "Bank name " << pbh->name << std::endl;
			if (strncmp(pbh->name, "DTRG", 4) == 0) {
				
				//int TriggerUsed = ((pdata[2] & 0x0000FF00) >> 8);
				int TriggerUsed = ((pdata[2] & 0x00FF0000) >> 16);
				int Timestamp = (pdata[0]);
				return std::pair<unsigned int,unsigned int>(TriggerUsed,Timestamp);
			}				
		}
	} while (bksize);		

	
	return std::pair<unsigned int,unsigned int>(-1,-1);
}


