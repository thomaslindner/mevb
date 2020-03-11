/*****************************************************************************/
/**
\file ebFragment.hxx

## Contents

This file contains the class definition for the event builder class
 *****************************************************************************/

#ifndef EBFRAGMENT_HXX_INCLUDE
#define EBFRAGMENT_HXX_INCLUDE

#include <stdio.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <assert.h>
#include <stdlib.h>
#include <sys/time.h>
#include <atomic>
#include <algorithm> //for std::sort()

#include "midas.h"
#include "msystem.h"



/**
 * Contains all the methods necessary to:
 *
 * - Connect/disconnect the board through an optical connection
 * - Initialize the hardware (set the registers) for data acquisition
 * - Read and write to the ODB
 */
class EBFragment
{

public:

  /* Enums/structs */
  enum ConnectErrorCode {
    ConnectSuccess,
    ConnectErrorComm,
    ConnectErrorTimeout,
    ConnectErrorAlreadyConnected
  };

  struct EBFRAGMENT_CONFIG_SETTINGS {
    char      buffername[32];          //!< Buffer Name
    WORD      evid;                    //!< Event Id
    WORD      tmsk;                    //!< Trigger Mask
    BOOL      enable;                  //!< Enable fragment
  } config;   //!< instance of config structure

  /* Static */
  static const char *config_str_fragment[]; //!< Configuration string for this buffer
  static const char history_settings[][NAME_LENGTH];

  /* Constructor/Destructor */
  EBFragment(HNDLE hDB);

  /* Use move instead of copy semantics as we only need one copy of each
   * object (C++11).  See notes in implementation. */
  EBFragment(EBFragment&&) noexcept;

  EBFragment& operator=(EBFragment&&) noexcept;
  bool operator<(const EBFragment&) const;


  ~EBFragment();

  /* Public methods */
  ConnectErrorCode Connect();          //!< EB to Fragment Buffer
  ConnectErrorCode Connect(int, int);  //!< EB to Fragment Buffer
  static void * connectThread(void *); //!< To thread
  struct thread_args {
    EBFragment * ebfragment;
    pthread_cond_t * cv;
  };

  std::string connectStatusMsg;
  bool Disconnect();                 //!< EB to Fragment Buffer
  bool IsEnabled();                  //!< Fragment enabled
  bool IsRunning();                  //!<
  int GetBMBufferLevel(int);         //!< bm buffer level in bytes
  bool ReadFragment(void *);         //!< Read event from buffer
  DWORD GetSNFragment(void);         //!< Get current fragment event serial number
  int BankListOfFragment(void *);                      //!< Print Bank listing
  bool FetchHeaderNextEvent(uint32_t * header);  //!<
  bool DeleteNextEvent();                        //!<
  bool FillStatBank(char *, suseconds_t);        //!<
  bool FillBufferLevelBank(char *);              //!<
  bool AddBanksToEvent(char * pevent);   //!<
  bool FillEventBank(char * pevent);             //!<
  bool GetV1720Fragment(void **, DWORD * dtmtsl, DWORD * dtmtsh, DWORD ** qhisto);
  bool Poll(DWORD*);                                            //!<
  int SetFragmentRecord(HNDLE h);                               //!<
  int SetHistoryRecord(HNDLE h, void(*cb_func)(INT,INT,void*)); //!<
  int InitializeForAcq();                                       //!<

  /* Getters/Setters */
  int GetEvID() { return (int) evid_; }                    //!< returns buffer EVID
  void SetEvID(unsigned short evid) { evid_ = evid; }      //!< set EVID

  int GetTmask() { return (int) tmsk_; }                   //!< returns buffer Trigger Mask
  void SetTmask(unsigned short tmask) { tmsk_ = tmask; }   //!< set Trigger mask

	int GetDtmTmask() { return (int) dtmtmsk_; }                   //!< returns DTM Trigger Mask ID
  void SetDtmTmask(int dtmtmask) { dtmtmsk_ = dtmtmask; }   //!< set DTM Trigger mask ID

  BOOL GetEnable() { return enable_; }                     //!< returns buffer enable
  void SetEnable(bool frage) { enable_ = frage; }          //!< Set fragment enable flag

  int GetFragmentID() { return (int) fragmentID_; }                    //!< returns thread fragmentID
  void SetFragmentID(int fragmentID) { fragmentID_ = fragmentID; }     //!< set fragmentID

  std::string GetName();

  void SetBufferName(const char * str) { buffer_name_ = str; }
  std::string GetBufferName() { return buffer_name_; }

  void SetFrontEndName(const char * str) { fe_name_ = str; }
  std::string GetFrontEndName() { return fe_name_; }

  void SetEqpName(const char * str) { eqp_name_ = str; }
  std::string GetEqpName() { return eqp_name_; }

  int GetBufferHandle() { return buffer_handle_; }  //!< returns buffer device handle
  void SetBufferHandle(int bh) { buffer_handle_ = bh; }

   HNDLE GetODBHandle() { return odb_handle_; }      //!< returns main ODB handle

  HNDLE GetSettingsHandle() { return setting_frag_handle_; }   //!< returns settings record handle

  bool GetSettingsTouched() { return settings_touched_; }  //!< returns true if odb settings  touched
  void SetSettingsTouched(bool t) { settings_touched_ = t; }  //!< set _settings_touched

  int GetRingBufferHandle() { return rb_handle_; }       //!< returns ring buffer index
  void SetRingBufferHandle(int rb_handle) { rb_handle_ = rb_handle; } //!< set ring buffer index

  int GetNumEventsInRB() { return num_events_in_rb_.load(); }  //!< returns number of events in ring buffer

  int GetRequestID() { return requestID_; }                    //!< returns BM request ID
  void SetRequestID(int requestID){ requestID_ = requestID; }

  int GetVerbosity(){ return verbosity_; }
  void SetVerbosity(int verbosity){ verbosity_ = verbosity; }

  int GetThreadStatus(){ return thread_status_; }
  void SetThreadStatus(int status){ thread_status_ = status; }

  int GetRebinFactor() { return fRebinFactor; }
  void SetRebinFactor(int rebinFactor) { fRebinFactor = rebinFactor; }

	/// This method only applies to the DTM fragment.  It will scan the DTM trigger mask used
	/// from next event in ring buffer.  Also returns the timestamp.
	/// Format is std::pair< trigger_mask, timestamp >
	///  Return -1 indicates failure
	std::pair<unsigned int,unsigned int> GetDTMTriggerMaskUsed();

  /* These are atomic with sequential memory ordering. See below */
  void IncrementNumEventsInRB() { num_events_in_rb_++; }      //!< Increment Number of events in ring buffer
  void DecrementNumEventsInRB() { num_events_in_rb_--; }      //!< Decrement Number of events in ring buffer

	void PrintSome(){
	
	// Make sure this is DTM fragment
		std::cout << "Print " << GetTmask() << std::endl;
	
		std::cout << "Num events  " << GetNumEventsInRB() << std::endl;
	
		// Now grab the event from the ring
		char * src;
		// the src is in the rb and contains a full Midas event
		int status = rb_get_rp(this->GetRingBufferHandle(), (void**)&src, 1000);
		if (status == DB_TIMEOUT) {
			cm_msg(MERROR,"GetDTMTriggerMaskUsed", "Got rp timeout for fragmentID %d", this->GetFragmentID());
			return ;
		}
		
		DWORD *pdata = (DWORD*)src;

		for(int i = 0; i < 600; i++){
			std::cout << std::dec << i << ":" << std::hex << *(pdata+i-300) << " ";
			//if(i == 5) std::cout << std::endl;
		}
		std::cout << std::dec << std::endl;
	}
	
	void ResetTimeDiff(){
		fTimeStampDiffSet = false;
		fTimeStampDifference = 0xdeadbeef;
		fTimeStampErrors = 0;

		fLastTimeReadEvent = 0;
		fLastTimeReadEventWarn = false;
		fLastTimeReadEventError = false;
	}

private:

  /* Private fields */

  /* IMPORTANT
   *
   * If adding additional fields, do NOT forget to change the move constructor
   * and move assignment operator accordingly
   */
  unsigned short evid_         //!< Buffer EVID
                ,tmsk_;        //!< Buffer Trigger mask
	int dtmtmsk_;                //!< DTM trigger mask ID for this front-end
  bool enable_;                //!< Equipment Buffer Enable flag
  int fragmentID_;             //!< may not be used or should be independent vector
  std::string buffer_name_;    //!< Buffer name
  std::string fe_name_;        //!< Frontend program name
  std::string eqp_name_;       //!< Equipment name
  int buffer_handle_;          //!< Buffer Handle
  int requestID_;              //!< Request Id for this fragment (evid/tmsk,handle)
  HNDLE odb_handle_;           //!< main ODB handle
  HNDLE setting_frag_handle_;  //!< Handle for the device settings record
  int rb_handle_;              //!< Handle to ring buffer
  bool settings_loaded_;       //!< ODB settings loaded, may not be used
  //Todo: Add hot-link on the enable
  bool settings_touched_;      //!< ODB settings touched, only for enable
  bool running_;               //!< Thread running, run in progress
  int verbosity_;              //!< Make the driver verbose
                               //!< 0: off
                               //!< 1: normal
                               //!< 2: very verbose
	int thread_status_;          //!< Thread status: 1 = fragment has running thread; 0 = fragment does not have running thread.

	bool fTimeStampDiffSet;            //!< Whether or not we have defined timestamp difference.
	unsigned int fTimeStampDifference; //!< This is the difference between the time stamp for the first
	                                  //!< for this fragment vs the DTM fragment.
	int fTimeStampErrors; //!< Number of time stamp errors for this fragment

	DWORD fLastTimeReadEvent; //!< Keep track of the last time we successfully read an event from event buffer (unix time)
	                        //!< Used to detect front-ends that died... 
	bool fLastTimeReadEventWarn;
	bool fLastTimeReadEventError;
	
	int fRebinFactor; //!< Number of 4ns bins to combine into for the summary QT histogram


  /* We use an atomic types here to get lock-free (no pthread mutex lock or spinlock)
   * read-modify-write. operator++(int) and operator++() on an atomic<integral> use
   * atomic::fetch_add() and operator--(int) and operator--() use atomic::fetch_sub().
   * The default memory ordering for these functions is memory_order_seq_cst (sequentially
   * consistent). This saves us from inserting a memory barrier between read/write pointer
   * incrementation and an increment/decrement of this variable.   */
  std::atomic<int> num_events_in_rb_;  //!< Number of events stored in ring buffer

  /* Private methods */

	// Method to check if the control word in the ring buffer is correctly set.
	// rbp points to the start of this ring buffer event.
	bool CheckControlWord(char *rbp);


};

#endif // EBFRAGMENT_HXX_INCLUDE

