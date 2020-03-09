/****************************************************************************/
/**
\file feBuilder.cxx

\mainpage

\section contents Contents
Event Builder based on the Midas framework

\subsection notes Notes about this application

This application is a multi-threaded Event builder.

Equipment with the "EQ_EB" flag will be register as potential source to the
event builder task. Each of these equipment will provide a "fragment" retrieved 
from the Midas buffer associated to that equipment. The task of reading the 
Midas event buffer is farmed to an individual thread.

Each thread is in charge of reading the individual Midas event and place it 
it in its corresponding fragment ring buffer.

The main task will collect all the fragments from their respective ring buffer,
check for proper assembly based on a particular matching condition and copy all the fragments 
into a final Midas event buffer (SYSTEM) after having stripped all the fragment
event headers.

Possible matching condition:

a) Serial Number matching: final event will be composed if and only if all the 
   active fragments have a matching serial event number. Otherwise the task will be aborted.

b) Time Stamp matching & trigger mask: final event will be composed if and only if
   all the expected fragments (defined in a trigger fragment with a trigger mask) 
   have a matching time stamp in a dedicated bank from each fragment.

\subsubsection threadProcessing Possible inline data processing
In the individual thread (fragment thread) it is possible to process the fragment 
data for multi-level trigger condition evaluation. This information is to be added
to the fragment ring buffer.

\subsubsection mainThreadProcessing 
In the main thread (collector thread), it is possible after confirmation of 
Time Stamp matching to evaluate the "extra fragment information" in order to
dynamically change the composition of the final event.

- feBuilder.exe

 *************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/resource.h>

#include <fstream>
#include <string>
//#include <mfe.h>
#include <vector>
#include <memory>

#include "midas.h"
#include "ebFragment.hxx"


// __________________________________________________________________
// --- General feBuilder parameters

#define USE_SYSTEM_BUFFER
#define EBUILDER_EQUIPMENT 0
#define EQ_NAME        "EBuilder"

#ifndef NBBCORES
#define NBCORES          2   //!< Number of cpu cores, for process/thread locking
#endif

#ifndef NBFRAGMENT
#define NBFRAGMENT      10
#endif

#define BM_BUFFER_SIZE  1000000
#define SN_MODE 1
#define TS_MODE 2

#ifndef HWLOGDIR
#define HWLOGDIR "/home/deap/pro/FrontEnd/ebuilder"
#endif

#define  EQ_EVID   1                //!< Event ID
#define  EQ_TRGMSK 0                //!< Trigger mask
#define  FE_NAME   "febuilder"      //!< Frontend name

#define UNUSED(x) ((void)(x)) //!< Suppress compiler warnings
//#define DEBUGTHREAD
//#define SYNCEVENTS
const bool SYNCEVENTS_DEBUG = true;

// __________________________________________________________________
// --- MIDAS global variables
extern HNDLE hDB;   //!< main ODB handle
extern BOOL debug;  //!< debug printouts

/* make frontend functions callable from the C framework */
#ifdef __cplusplus
extern "C" {
#endif

/*-- Globals -------------------------------------------------------*/

//! The frontend name (client name) as seen by other MIDAS clients
char *frontend_name = (char*)FE_NAME;
//! The frontend file name, don't change it
char *frontend_file_name = (char*)__FILE__;
//! frontend_loop is called periodically if this variable is TRUE
BOOL frontend_call_loop = FALSE;
//! a frontend status page is displayed with this frequency in ms
INT display_period = 000;
//! maximum event size produced by this frontend
//INT max_event_size = 32 * 34000 * 2 *2;
INT max_event_size = 	3600000;
//! maximum event size for fragmented events (EQ_FRAGMENTED)
INT max_event_size_frag = 5 * 1024 * 1024 * 4 *2;
//! buffer size to hold events
INT event_buffer_size = 25 * max_event_size + 10000;

INT _mode = 0;     //!< Assembly mode, 1: Serial Number assembly, 2: Time Stamp assembly (user code)
INT _modulo=0;     //!< Modulo factor for event distribution

//! log of hardware status
std::ofstream hwlog;
std::string hwlog_filename;

bool runInProgress = false; //!< run is in progress
bool eor_transition_called = false;// keep track of where cm_transition(TR_STOP...) has been called.
bool timestampErrorWarning = false;// warn user about timestamp errors
BOOL fStrictTimestampMatching = true; // determine whether to stop run for timestamp mismatchs

// __________________________________________________________________
/*-- MIDAS Function declarations -----------------------------------------*/
INT frontend_init();
INT frontend_exit();
INT begin_of_run(INT run_number, char *error);
INT end_of_run(INT run_number, char *error);
INT pause_run(INT run_number, char *error);
INT resume_run(INT run_number, char *error);
INT frontend_loop();
extern void interrupt_routine(void);  //!< Interrupt Service Routine


INT TSDeapAssembly(char *pevent, INT off);
INT SNAssembly(char *pevent, INT off);
INT read_buffer_level(char *pevent, INT off);
void * fragment_thread(void *);

// __________________________________________________________________
/*-- Equipment list ------------------------------------------------*/
//! Main structure for midas equipment
EQUIPMENT equipment[] =
{
 {
   EQ_NAME,                     /* equipment name */
     {
        EQ_EVID, EQ_TRGMSK,     /* event ID, trigger mask */
#ifdef USE_SYSTEM_BUFFER
       "SYSTEM",                /* write events to system buffer */
#else
       "EBBUF",                 /* make different frontends (indexes) write to different buffers */
#endif //USE_SYSTEM_BUFFER
       EQ_POLLED,               /* equipment type */
       LAM_SOURCE(0, 0x0),      /* event source crate 0, all stations */
       "MIDAS",                 /* format */
       TRUE,                    /* enabled */
       RO_RUNNING,              /* read only when running */
       500,                     /* poll for 500ms */
       0,                       /* stop run after this event limit */
       0,                       /* number of sub events */
       0,                       /* don't log history */
       "", "", ""
     },
   TSDeapAssembly,       /* readout routine */
   //SNAssembly,       /* readout routine */
 },
 
 {
   "EBlvl",                /* equipment name */
   {
     2, 0,                   /* event ID, trigger mask */
     "SYSTEM",               /* event buffer */
     EQ_PERIODIC,   /* equipment type */
     0,                      /* event source */
     "MIDAS",                /* format */
     TRUE,                   /* enabled */
     RO_RUNNING | RO_TRANSITIONS |   /* read when running and on transitions */
     RO_ODB,                 /* and update ODB */
     1000,                  /* read every 1 sec */
     0,                      /* stop run after this event limit */
     0,                      /* number of sub events */
     1,                      /* log history */
     "", "", ""
   },
   read_buffer_level,       /* readout routine */
 },
 
 {""}
};
  
#ifdef __cplusplus
}
#endif

std::vector<EBFragment> ebfragment;               //!< objects for each fragment
std::vector<EBFragment>::iterator itebfragment;   //!< Main thread iterator


pthread_t tid[NBFRAGMENT];                 //!< Thread ID
int thread_retval[NBFRAGMENT] = {0};       //!< Thread return value
int thread_fragment[NBFRAGMENT];           //!< fragment number associated with each thread

/********************************************************************/
/********************************************************************/
/********************************************************************/

/**
 * \brief   ODB callback info.
 *          For Ebuilder, there is no hot-link.
 *          At BOR, the Enable flag will be check for updated value instead
 *
 * Function which gets called when record is updated
 *
 * \param   [in]  h main ODB handle
 * \param   [in]  hseq Handle for record that was updated
 * \param   [in]  info Record descriptor additional info
 */
void seq_callback(INT h, INT hseq, void *info){
   KEY key;

   for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
      if (hseq == itebfragment->GetSettingsHandle()){
         db_get_key(h, hseq, &key);
         itebfragment->SetSettingsTouched(true);
         cm_msg(MINFO, "seq_callback", "Settings %s touched. Changes will take effect at start of next run.", key.name);
      }
   }
}

//---------------------------------------------------------------------------------
/**
 * \brief   Frontend initialization
 *
 * Runs once at application startup.
 * Scan the Equipment frontend for EQ_EB flag
 * Save under settings/eqname/ minimal eqp info
 * Create Ebuilder global var
 * Set CPU to core 0
 * Done, the rest is in BOR
 *
 * \return  Midas status code
 */
INT frontend_init() {
  INT i, size, type;
  KEY key;
  HNDLE hEqKey, hSubkey;

  //   std::stringstream * ss = new std::stringstream();
  //  *ss << HWLOGDIR << "/hwlog" << ((feIndex == -1) ? 0 : feIndex);
  //  hwlog_filename = ss->str();
  
  set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Initializing...", "#FFFF00");
  printf("<<< Begin of Init\n");
  
  // Set early start and late stop transition number
  cm_set_transition_sequence(TR_START, 499);
  cm_set_transition_sequence(TR_RESUME, 499);
  cm_set_transition_sequence(TR_STOP, 520);
  cm_set_transition_sequence(TR_PAUSE, 520);
  
  
  // --- Suppress watchdog for now
  cm_set_watchdog_params(FALSE, 0);
  
  // Get Eq key
  if (db_find_key(hDB, 0, "Equipment", &hEqKey) != DB_SUCCESS) {
    cm_msg(MINFO, "load_fragment", "Equipment listing not found");
    return 0;
  }
  
	/* Read & Build the fragment list by scanning the ODB /Equipment tree
	 */
	for (i = 0 ;; i++) {
		unsigned short trigger_mask, event_id;
		const char bufname[NAME_LENGTH]={""};
		const char fename[NAME_LENGTH]={""};
		db_enum_key(hDB, hEqKey, i, &hSubkey);
		if (!hSubkey)  break;
		db_get_key(hDB, hSubkey, &key);
		// Go in the equipment directories
		if (key.type == TID_KEY) {
			/* Check if equipment is EQ_EB */
			size = sizeof(INT);
			db_get_value(hDB, hSubkey, "common/type", &type, &size, TID_INT, 0);
			if (type & EQ_EB) {
				size = sizeof(fename);
				db_get_value(hDB, hSubkey, "common/Frontend name", (void *)fename, &size, TID_STRING, 0);
				size = sizeof(bufname);
				db_get_value(hDB, hSubkey, "common/Buffer", (void *)bufname, &size, TID_STRING, 0);
				size = sizeof(WORD);
				db_get_value(hDB, hSubkey, "common/Trigger Mask", &trigger_mask, &size, TID_WORD, 0);
				size = sizeof(WORD);
				db_get_value(hDB, hSubkey, "common/Event ID", &event_id, &size, TID_WORD, 0);

				// Inserts a new object at the end of the vector
				ebfragment.push_back(hDB);
				// .back(): Returns a reference to the last element in the vector
				// Fill the object Fragment
				ebfragment.back().SetVerbosity(0);
				ebfragment.back().SetEqpName(key.name);      // Equipment name
				ebfragment.back().SetFrontEndName(fename);   // Front-end name
				ebfragment.back().SetBufferName(bufname);    // Buffer name
				ebfragment.back().SetTmask(trigger_mask);    // Trigger Mask
				ebfragment.back().SetEvID(event_id);         // Event ID

				if (debug) {
					printf(" - Fragment %d - Eqp:%s,  evID %d, Tmask:0x%x, buffer %s\n"
						,i , ebfragment.back().GetEqpName().c_str()
						, ebfragment.back().GetEvID()
						, ebfragment.back().GetTmask()
						, ebfragment.back().GetBufferName().c_str());
				}
				//            ebfragment.back().SetFragmentRecord(hDB);

			} else { // Equipment not involved in the EBuilder
				if (debug) printf("\r");
			}
		}
	} // for loop over odb enumeration

	if (debug) {
		printf("Number of objects: ebfragment.size()=%ld\n", ebfragment.size());
		for (itebfragment=ebfragment.begin() ; itebfragment!=ebfragment.end(); ++itebfragment) {
			printf("Before ordering         - Equipment name: %s,  evID %d, Tmask:0x%x, buffer %s\n"
					, itebfragment->GetEqpName().c_str()
					, itebfragment->GetEvID()
					, itebfragment->GetTmask()
					, itebfragment->GetBufferName().c_str());
		}
	}

	/* Sort the ebfragment vector correctly once for all.
	 *  Sort fragment object based on the trigger mask.
	 *  The DTM fragment should be first and will always be present in the every
	 *  assembled event.
	 *  DTM Event  ID = 1, trigger mask = 0x0001
	 *  V1720-0    ID = 1, trigger mask = 0x0002
	 *  V1720-1    ID = 1, trigger mask = 0x0004
	 *  V1720-2    ID = 1, trigger mask = 0x0008
	 *  V1720-3    ID = 1, trigger mask = 0x0010
	 *  V1740      ID = 1, trigger mask = 0x0020
	 *  V1740-VETO ID = 1, trigger mask = 0x0040
	 *  Sort objects based on Tmask from low to high values
	 */
	std::sort(ebfragment.begin(), ebfragment.end());
  
  if (debug) {
    printf("\n");
    printf("Number of objects: ebfragment.size()=%ld\n", ebfragment.size());
  }
  
  // Setup ODB record for each eqp having EQ_EB, they should be in order L->H
  for (itebfragment=ebfragment.begin() ; itebfragment!=ebfragment.end(); ++itebfragment) {
    itebfragment->SetFragmentRecord(hDB);
    if (debug) {
      printf("Ordered by trigger mask - Equipment name: %s,  evID %d, Tmask:0x%x buffer %s Enable:%d\n"
	     , itebfragment->GetEqpName().c_str()
	     , itebfragment->GetEvID()
	     , itebfragment->GetTmask()
	     , itebfragment->GetBufferName().c_str()
	     , itebfragment->GetEnable());
    }
  }
  
  // No more fragment to register
  if (ebfragment.size() > 1)
    printf("Found %ld fragments for event building\n", ebfragment.size());
  else
    printf("Found one fragment for event building\n");
  
  /* Create if not present Ebuilder Global var under /settings/ in ODB
   * For now only 3 parameters, may contain time stamp window, Qthreshold, ...
   * may be should be moved in its own dir /settings/control/...
   */
  HNDLE hsf;
  char set_str[200];
  sprintf(set_str, "%s/Settings", EQ_NAME);
  db_find_key(hDB, hEqKey, set_str, &hsf);
  
  size = sizeof(INT);
  db_get_value(hDB, hsf
	       , "Assembly mode", &_mode, &size, TID_INT, TRUE);  // Create if not present
  size = sizeof(INT);
  db_get_value(hDB, hsf
	       , "Modulo", &_modulo, &size, TID_INT, TRUE);  // Create if not present
  
  // CPU core allocation (0 for the main thread)
  //cpu_set_t mask;
  //CPU_ZERO(&mask);
  //CPU_SET(0, &mask);  //Main thread to core 0
  //if( sched_setaffinity(0, sizeof(mask), &mask) < 0 ) {
  //printf("ERROR setting cpu affinity for main thread: %s\n", strerror(errno));
  //}
  
  std::cout << "Finished initialization"<<std::endl;
  // web status
  set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Initialized", "#00ff00");
  return SUCCESS;
}						

//---------------------------------------------------------------------------------
/**
 * \brief   Frontend exit
 *
 * Runs at frontend shutdown.  Disconnect hardware and set equipment status in ODB
 *
 * \return  Midas status code
 */
INT frontend_exit(){

   set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Exiting...", "#FFFF00");

   for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
	   itebfragment->Disconnect();
   }

   set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Exited", "#00ff00");
   return SUCCESS;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Thread Cleanup
 *
 * Method to clean up threads and buffers if we have an abort in the middle of begin_of_run().
 *
 * \param   [out] error code
 */
int thread_cleanup(){
	
	int status1;
	int * status;

	cm_msg(MINFO,"thread_cleanup", "Cleanup all the threads/buffers from aborted started.");
				
	// This will exit the threads
	runInProgress = false;

	for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
		if (! itebfragment->IsEnabled()) continue;   // Skip disabled fragment
		
				// If thread status = 0, then thread never started.  Nothing to do.
		if(itebfragment->GetThreadStatus() == 0) continue;

		pthread_join(tid[itebfragment->GetFragmentID()],(void**)&status);
				
		// Reset thread status
		itebfragment->SetThreadStatus(0);

		// Remove event requestID		
		//		cm_msg(MINFO,"EOR", "DElete request (%i) %s",itebfragment->GetFragmentID(),itebfragment->GetEqpName().c_str());
		status1 = bm_delete_request(itebfragment->GetRequestID());
		if (status1 != BM_SUCCESS) {
			cm_msg(MERROR, "EOR", "Delete buffer[%d] stat:", status1);
			return status1;
		}
		itebfragment->SetRequestID(-1);
		
		//cm_msg(MINFO,"EOR", "Close buffer (%i)",itebfragment->GetFragmentID());
		// Close source buffer
		status1 = bm_close_buffer(itebfragment->GetBufferHandle());
		if (status1 != BM_SUCCESS) {
			cm_msg(MERROR, "sEOR", "Close buffer[%d] stat:", status1);
			return status1;
		}
		itebfragment->SetBufferHandle(-1);
		
		// Delete Ring Buffer
		rb_delete(itebfragment->GetRingBufferHandle());
		itebfragment->SetRingBufferHandle(-1);
		
		// Zero out counter
		if(itebfragment->GetNumEventsInRB() > 0){
			while(itebfragment->GetNumEventsInRB() != 0){
				itebfragment->DecrementNumEventsInRB(); //atomic									 
			}
			
		}
	}	

	return 1;
}


//---------------------------------------------------------------------------------
/**
 * \brief   Begin of Run
 *
 * Called every run start transition.
 * Read ODB settings
 * Connect to fragment buffer
 * Create RingBuffer for each enabled fragment
 * Create threads for enabled fragment
 *
 * \param   [in]  run_number Number of the run being started
 * \param   [out] error Can be used to write a message string to midas.log
 */
INT begin_of_run(INT run_number, char *error) {
  
  hwlog.open(hwlog_filename.c_str(), std::ios::app);
  hwlog << "========================================================= BOR: (RUN #: "<< run_number << " TIME = " << ss_time() << ") ===================================================" << std::endl;
  hwlog.close();
  
  set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Starting run...", "#FFFF00");
  cm_msg(MINFO,"BOR", "Start of begin_of_run");
  printf("<<< Start of begin_of_run\n");
  
  int rb_handle;
  int status1, status2;
  
  // Grab the current overall settings
  HNDLE hsf;
  char set_str[200];
  int size;
  sprintf(set_str, "/Equipment/%s/Settings", EQ_NAME);
  db_find_key(hDB, 0, set_str, &hsf);
  
  // Get the DTM->FE trigger mask map.  Nominally the mapping will be
  // "DTM Trigger Mask Used"  "FE Trigger Mask ID"
  // 1                                        -1   (no front-end for this DTM NIM output )
  // 2                                       0x20  (V1740 front-end mask)
  // 4                                       0x1e  (V1720 mask (bitwise OR of all of them)
	// 8                                       0x4    (VETO mask)
	int dtm_fe_trigger_mask_map[8];
	size = sizeof(dtm_fe_trigger_mask_map);
	db_get_value(hDB, hsf
							 , "DTM2FETriggerMaskMap", &dtm_fe_trigger_mask_map, 
							 &size, TID_INT, TRUE);  // Create if not present

  // Get the binning for the QT summary histogram, which will be passed to
	// each fragment.
  int rebin_factor = 0;
  size = sizeof(rebin_factor);
  db_get_value(hDB, hsf, "QT summary rebin factor", &rebin_factor, &size, TID_INT, TRUE);

  // Get the ODB variable that determines whether to stop the run for timestamp mismatchs. 
  size = sizeof(fStrictTimestampMatching); 
  db_get_value(hDB, hsf, "strictTimestampMatching",&fStrictTimestampMatching, &size, TID_BOOL, TRUE);
  
  
  /* local flag indicating that a run is in progress
   * Todo: need to check the escape condition...
   */
  runInProgress = true;
  
  // flag to ensure we only stop run once if we have timestamp mismatchs
  eor_transition_called = false;
  
  timestampErrorWarning = false;
  
  // Per found fragment in the ODB equipment list
  for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
    
    // Reset thread status
    itebfragment->SetThreadStatus(0);
    
    // Update Enable flag from ODB
    int bsize = sizeof(BOOL);
    bool fragE;
    int status = db_get_value(itebfragment->GetODBHandle()
                              , itebfragment->GetSettingsHandle(), "enable", &fragE, &bsize, TID_BOOL, FALSE);
    itebfragment->SetEnable(fragE);
    if (! itebfragment->IsEnabled()) {
      cm_msg(MINFO, "ebuilder", "Fragment %s disabled", itebfragment->GetEqpName().c_str());
      continue;   // Skip disabled fragment
    }
    
    int check_fragment_running = cm_exist(itebfragment->GetFrontEndName().c_str(), TRUE);
    if(check_fragment_running != CM_SUCCESS){
      cm_msg(MERROR, "feBuilder:BOR", "Event Builder Fragment %s (program %s) is not running. Not allowed: abort run. Disable this EB fragment or start front-end."
             , itebfragment->GetEqpName().c_str(),itebfragment->GetFrontEndName().c_str());			
      set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
      thread_cleanup();
      return BM_CONFLICT;
      
    }
    
    // Make sure the fragment buffer is not 'SYSTEM'.  That will presumably cause problems.
    if(strcmp (itebfragment->GetBufferName().c_str(),"SYSTEM") == 0){
      cm_msg(MERROR, "feBuilder:BOR", "Event Builder Fragment %s is writing to SYSTEM buffer. Not allowed: abort run. Disable this EB fragment."
             , itebfragment->GetEqpName().c_str());			
      set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
      thread_cleanup();
      return BM_CONFLICT;
    }
    
    // Set the binning for the QT summary histogram
    itebfragment->SetRebinFactor(rebin_factor);
    
    // Connect to fragment buffer
    int bh;
    status1 = bm_open_buffer((itebfragment->GetBufferName().c_str()), BM_BUFFER_SIZE, &bh);
    // Save buffer handle
    if (status1 == BM_SUCCESS) {
      itebfragment->SetBufferHandle(bh);
    } else {
      itebfragment->SetBufferHandle(-1);
    }
    
    if (debug) printf("bm_open_buffer:%d\n", itebfragment->GetBufferHandle());
    
    if (itebfragment->GetBufferHandle() >= 0) {
      // Register for specified channel event ID but all Trigger mask AND ALL EVENTS
      int rid;
      status2 = bm_request_event(itebfragment->GetBufferHandle(), itebfragment->GetEvID()
                                 , TRIGGER_ALL, GET_ALL, &rid, NULL);
      // Save event request ID
      if (status2 == BM_SUCCESS) {
        itebfragment->SetRequestID(rid);
      } else {
        itebfragment->SetRequestID(-1);
      }
    }
    
    if (debug) printf("bm_request_event:%d\n", itebfragment->GetRequestID());
    
    // Handle connection to buffer error
    if (((status1 != BM_SUCCESS) && (status1 != BM_CREATED)) ||
        ((status2 != BM_SUCCESS) && (status2 != BM_CREATED))) {
      cm_msg(MERROR, "BOR_booking", "Open buffer/event request failure %s [%d %d]"
             , itebfragment->GetEqpName().c_str(), status1, status2);
      set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
      thread_cleanup();
      return BM_CONFLICT;
    }
    
    // Create ring buffer for fragment
    status = rb_create(event_buffer_size, max_event_size, &rb_handle);
    printf("Ring buffer size: %i ; max event: %i\n",event_buffer_size, max_event_size);
    if(status == BM_SUCCESS) {
      itebfragment->SetRingBufferHandle(rb_handle);
      if (debug) printf("rb_create_event:%d\n", itebfragment->GetRingBufferHandle());
      
    } else {
      cm_msg(MERROR, "feBuilder:BOR", "Failed to create rb for fragment %s"
             , itebfragment->GetBufferName().c_str());
      set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
      thread_cleanup();
      return BM_CONFLICT;
    }
    
    //Create one thread per fragment
    int fid = itebfragment - ebfragment.begin();
    status = pthread_create(&tid[fid], NULL, &fragment_thread, (void*)&*itebfragment);
    if(status) {
      cm_msg(MERROR,"feBuilder:BOR", "Couldn't create thread for fragment %d. Return code: %d"
             , fid, status);
      set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
      thread_cleanup();
      return SS_ABORT;
    }
    
    // Register FragmentID for all the fragments -> make a thread even for the 1st one
    itebfragment->SetFragmentID(fid);
    itebfragment->SetThreadStatus(1);
    
    // Register the correct DTM trigger mask id for this fragment.  
    int dtm_trigger_mask_id = -1;
    for(int i = 0; i < 8; i++){
      if(dtm_fe_trigger_mask_map[i] >= 0 &&
         dtm_fe_trigger_mask_map[i] & itebfragment->GetTmask()){
        dtm_trigger_mask_id = (1<<(i));				
      }
    }
    itebfragment->SetDtmTmask(dtm_trigger_mask_id);
    // Reset the timestamp difference between this fragment and the DTM fragment.
    itebfragment->ResetTimeDiff();
    
    if (debug) {
      printf(" pthread_create, FragmentID:%d\n", itebfragment->GetFragmentID());
      printf(" buffer name:%s\n", itebfragment->GetBufferName().c_str());
      printf(" buffer handler:%2d\n", itebfragment->GetBufferHandle());
      printf(" bm_open status: %d\n", status1);
      printf(" event request ID:%2d\n", itebfragment->GetRequestID());
      printf(" event request status: %d\n", status2);
      printf(" event id:%2d\n", itebfragment->GetEvID());
      printf(" trigger mask:0x%4.4x\n", itebfragment->GetTmask());
    }
  }  // for fragment
  
  // Done
  set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Started run", "#00ff00");
  
  
  return SUCCESS;

}

//---------------------------------------------------------------------------------
/**
 * \brief   Fragment thread
 * Read event from defined BM_buffer (through arg) and write event to dedicated Ring Buffer
 * Extract TimeStamp min/max from the 8 QTbanks
 * Compose QvsT histogram from all the pulses of all the banks, of all the modules.
 * Append QvsT histo + TS min/max to the Ring Buffer 
 *
 * \param   [in]  arg thread number
 * \param   [out] none
 */
void * fragment_thread(void * arg) {
  
  EBFragment * pebfragment = (EBFragment *) arg;
  int rb_handle = pebfragment->GetRingBufferHandle();
  int fragment = pebfragment->GetFragmentID();
  void *wp;
  int status;
  int rb_level;

  /* Fragments have been sorted to have the TimeStamp fragment "Trigger fragment" first
   * The "Trigger fragment" will be dealt in the main thread during the event assembly
   */
  
  // Todo: Code to be checked
  // Lock each thread to a different cpu core
  //cpu_set_t mask;
  //CPU_ZERO(&mask);
  switch(NBCORES) {
  case 1:
    //Don't do anything
    break;
  case 2:
    //CPU_SET(fragment % 2, &mask); //TRIUMF test PC. Even buffer on core 0, odd buffer on core 1
    break;
  default:
    /* This will spread the threads on all cores except core 0 when the main thread resides.
     * ex 1 (SNOLAB): NBCORES=8, 4 threads:
     * threads (fragment) 0,1,2,3 will go on cores 1,2,3,4
     * ex 2: NBCORES 4, 4 threads:
     * threads (fragment) 0,1,2,3 will go on cores 1,2,3,1     */
    //CPU_SET((fragment % (NBCORES-1)) + 1, &mask);
    break;
  }
  
#if 0
  if( sched_setaffinity(0, sizeof(mask), &mask) < 0 ) {
    printf("ERROR setting cpu affinity for thread %d: %s\n", fragment, strerror(errno));
  }
#endif
  
  std::cout << "Started thread for "<< pebfragment->GetBufferName()<<"[" << fragment << "]" << std::endl;
  // Get rb handle
  rb_handle = pebfragment->GetRingBufferHandle();
  
  // THREAD loop dealing with a single fragment buffer
  // Assign the thread to the requested fragment object [1..7] -> [0..6] on pebfragment
  while(1) {
    

		/* If we've reached 75% of the ring buffer space, don't read
		 * the next event.  Wait until the ring buffer level goes down.
		 * It is better to let the front-end buffer and module buffers 
		 * fill up instead of the EB ring buffer.
		 */		
		rb_get_buffer_level(pebfragment->GetRingBufferHandle(), &rb_level);
		if(rb_level > (int)(event_buffer_size*0.75)){			
			usleep(1000);
			// Allow to break out in case where the ring buffer is still fulled, but run is stopped.
			if(!runInProgress)
				break;

			continue;
		}

		

    // Get wp for destination location
    status = rb_get_wp(rb_handle, &wp, 100);
    if (status == DB_TIMEOUT) {
      cm_msg(MERROR,"fragment_thread", "Got wp timeout for fragment %s (ID = %d)", pebfragment->GetEqpName().c_str(), pebfragment->GetFragmentID());
      thread_retval[fragment] = -1;
      pthread_exit((void*)&thread_retval[fragment]);
    }

    
    /* Main method (ReadFragment) for reading the event from its BUFFER and placing the data in
     * its corresponding RING BUFFER pointed with "wp", keep a pointer to the top of the Midas event
     * for further data processing.
     * This function also fills additional QvsT histograms and timestamps and appends them 
     * to the end of the event.
     */
		if(pebfragment->ReadFragment(wp) == BM_SUCCESS) {

			// Successfully read and processed event, so incrememnt number of event in ring buffer.
			pebfragment->IncrementNumEventsInRB(); //atomic

		} else {
	/* Do timeout as no event were available yet
	 *
	 */
			//Sleep for 100us to avoid hammering on the CPU Core
			usleep(1000);
		}
		
	
		// Allow to break out for exiting this thread
		if(!runInProgress)
			break;
	} // While forever
	
	//cm_msg(MINFO,"fragment_thread", "Exiting thread (%d) %s", fragment, pebfragment->GetEqpName().c_str());
	thread_retval[fragment] = 0;
	pthread_exit((void*)&thread_retval[fragment]);

}

//---------------------------------------------------------------------------------
/**
 * \brief   End of Run
 *
 * Called every stop run transition. Set equipment status in ODB,
 * stop acquisition on the modules.
 *
 * \param   [in]  run_number Number of the run being ended
 * \param   [out] error Can be used to write a message string to midas.log
 */
INT end_of_run(INT run_number, char *error)
{

	hwlog.open(hwlog_filename.c_str(), std::ios::app);
	hwlog << "========================================================= EOR: (RUN #: "<< run_number << " TIME = " << ss_time() << ") ===================================================" << std::endl;
	hwlog.close();

	set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ending run...", "#FFFF00");
	cm_msg(MINFO,"EOR", "Start of end_of_run");
	printf("<<< Start of end_of_run \n");

	//   DWORD eStored;
	int * status;
	int status1;

	if(runInProgress) {  //skip actions if we weren't running

		runInProgress = false;  //Signal threads to quit

		// Do not quit parent before children processes,
		// reunite the child to his parent before kill all
		//for (itebfragment = ebfragment.begin()+1; itebfragment != ebfragment.end(); ++itebfragment) {
		for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
			if (! itebfragment->IsEnabled()) continue;   // Skip disabled fragment

			pthread_join(tid[itebfragment->GetFragmentID()],(void**)&status);
			printf(">>> Thread %d joined, return code: %d\n", itebfragment->GetFragmentID(), *status);

			// Reset thread status
			itebfragment->SetThreadStatus(0);

			// Empty the remote BM buffers
			//	bm_empty_buffers();

			// Remove event requestID
			
			status1 = bm_delete_request(itebfragment->GetRequestID());
			if (status1 != BM_SUCCESS) {
				cm_msg(MERROR, "EOR", "Delete buffer[%d] stat:", status1);
				return status1;
			}
			itebfragment->SetRequestID(-1);

			// Close source buffer
			status1 = bm_close_buffer(itebfragment->GetBufferHandle());
			if (status1 != BM_SUCCESS) {
				cm_msg(MERROR, "sEOR", "Close buffer[%d] stat:", status1);
				return status1;
			}
			itebfragment->SetBufferHandle(-1);

			// Delete Ring Buffer
			rb_delete(itebfragment->GetRingBufferHandle());
			itebfragment->SetRingBufferHandle(-1);

			if(itebfragment->GetNumEventsInRB() > 0){
				cm_msg(MINFO,"EOR", "Warning: fragment %s (ID=%i) has >0 events left in ring buffer (%i)",
							 itebfragment->GetEqpName().c_str(),itebfragment->GetFragmentID(),itebfragment->GetNumEventsInRB());
		
				 while(itebfragment->GetNumEventsInRB() != 0){					 
					 itebfragment->DecrementNumEventsInRB(); //atomic									 
				 }

			}
		}
  }

	if(eor_transition_called){
		cm_msg(MERROR, "EndOfRun", "This run was stopped automatically because of timestamp mismatches in event builder. See early messages.");
	}else if(timestampErrorWarning){
    cm_msg(MERROR, "EndOfRun", "This run had timestamp mismatches in event builder.");
	}
  
  printf(">>> End Of end_of_run\n\n");
  set_equipment_status(equipment[EBUILDER_EQUIPMENT].name, "Ended run", "#00ff00");
  
  return SUCCESS;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Pause Run
 *
 * Called every pause run transition.
 *
 * \param   [in]  run_number Number of the run being ended
 * \param   [out] error Can be used to write a message string to midas.log
 *
 * \return  Midas status code
 */
INT pause_run(INT run_number, char *error)
{
  hwlog.open(hwlog_filename.c_str(), std::ios::app);
  hwlog << "========================================================= PAUSE: (RUN #: "<< run_number << " TIME = " << ss_time() << ") =================================================" << std::endl;
  hwlog.close();
  
  cm_msg(MINFO,"PAUSE", "Beginning of pause_run");
  printf("<<< Beginning of pause_run \n");
  
  cm_msg(MINFO,"PAUSE", "End of pause_run");
  printf("<<< End of pause_run \n");
  return SUCCESS;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Resume Run
 *
 * Called every resume run transition.
 *
 * \param   [in]  run_number Number of the run being ended
 * \param   [out] error Can be used to write a message string to midas.log
 *
 * \return  Midas status code
 */
INT resume_run(INT run_number, char *error)
{
  
  hwlog.open(hwlog_filename.c_str(), std::ios::app);
  hwlog << "========================================================= RESUME: (RUN #: "<< run_number << " TIME = " << ss_time() << ") ================================================" << std::endl;
  hwlog.close();
  
  cm_msg(MINFO,"RESUME", "Beginning of resume_run");
  printf("<<< Beginning of resume_run \n");
  
  cm_msg(MINFO,"RESUME", "End of resume_run");
  printf("<<< End of resume_run \n");
  return SUCCESS;
}

DWORD prevtime = 0;
INT numloops = 0;
DWORD sn=0;

//---------------------------------------------------------------------------------
/**
 * \brief   Frontend loop
 *
 * If frontend_call_loop is true, this routine gets called when
 * the frontend is idle or once between every event.
 *
 * \return  Midas status code
 */
INT frontend_loop()
{
  if((prevtime == 0) || ((ss_time() - prevtime) > 2)){
    
    hwlog.open(hwlog_filename.c_str(), std::ios::app);
    
    // Header, print when frontend starts
    if(prevtime == 0){
      hwlog << "=================================================================================================================================================" << std::endl;
      hwlog << "========================================================= FE START: (TIME = " << ss_time() << ") =========================================================" << std::endl;
      hwlog << "=================================================================================================================================================" << std::endl;
    }
    // Subheader, print every 80 lines
    
    hwlog.close();
    
    return SUCCESS;
  }
  return SUCCESS;
}

//---------------------------------------------------------------------------------
/********************************************************************	\
Readout routines for different events
\********************************************************************/
int Nloop;       //!< Number of loops executed in event polling
int Ncount;      //!< Loop count for event polling timeout
DWORD acqStat;   //!< ACQUISITION STATUS reg, must be global because read by poll_event, accessed by read_trigger_event
// ___________________________________________________________________
/*-- Trigger event routines ----------------------------------------*/
/**
 * \brief   Polling routine for events.
 *
 * \param   [in]  source Event source (LAM/IRQ)
 * \param   [in]  count Loop count for event polling timeout
 * \param   [in]  test flag used to time the polling
 * \return  1 if event is available, 0 if done polling (no event).
 * If test equals TRUE, don't return.
 */
extern "C" INT poll_event(INT source, INT count, BOOL test)
{
  register int i;
  
#if 1  // Look for DTM fragment; figure out which fragments are needed;
  // then wait till we have those fragments.
  for (i = 0; i < count; i++) {
    
    
    bool evtReady = true;
    
		
    // Check for data in DTM fragment (first fragment)
    itebfragment = ebfragment.begin();
    if (!itebfragment->GetEnable() || itebfragment->GetTmask() != 0x1){
      
      // Black magic madness.  We shouldn't ever get a different pointer with 
      // repeated calls to begin().  But somehow we do.
      // Strongly suggestive of deeper problem.  To be investigated! TL
      itebfragment = ebfragment.begin();
			bool enable = itebfragment->GetEnable();
			int mask =  itebfragment->GetTmask();
      if (!enable || mask != 0x1){
        std::string name = itebfragment->GetBufferName();
	
				cm_msg(MERROR, "poll_event", "DTM front-end not enabled; poll_event will fail! %i %i (now %i %i %s)",
							 enable,mask,itebfragment->GetEnable(),itebfragment->GetTmask(),name.c_str());
				usleep(100);
				continue;
      }
    }			
    
    // Check if we have DTM fragment; if not, keep looping.
    if(itebfragment->GetNumEventsInRB() == 0){
			usleep(100);
			continue;
		}
    
    // Grab the output mask from DTM bank.
    std::pair<unsigned int,unsigned int> result = itebfragment->GetDTMTriggerMaskUsed();
    int triggerMaskUsed = result.first;
    
    // Loop over other fragments.
    for(unsigned int ifrag = 1; ifrag < ebfragment.size(); ifrag++){
      
      // If the fragment is disabled, then ignore
      if (!ebfragment[ifrag].GetEnable()){continue;}
      
      // If the fragment is not requested by DTM bank, then ignore
      if(!(ebfragment[ifrag].GetDtmTmask() & triggerMaskUsed)){ continue; }
      
      // Now check if this fragment has available events in ring buffer
      if(ebfragment[ifrag].GetNumEventsInRB() == 0) {
	evtReady = false;				
      }
    }
    
    //If event not ready or we're in test phase, keep looping
    if (evtReady && !test){
      return 1;
    }
    //			cm_yield(0);
		usleep(100);
  }
#endif
  
  return 0;
}

//---------------------------------------------------------------------------------
INT SNAssembly(char *pevent, INT off)
{
  sn = SERIAL_NUMBER(pevent);
  
  if (!runInProgress) return 0;
  
  // Prepare event for MIDAS bank
  bk_init32(pevent);
  
  
  for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {
    if (debug) {
      printf("fragmentID:%d Enable:%d Name:%s\n", itebfragment->GetFragmentID()
	     , itebfragment->GetEnable()
	     , itebfragment->GetBufferName().c_str());
    }
    if (itebfragment->GetEnable()) {
      
      /*
       * CHECK for S/N across all the fragments
       */
      DWORD snref, sn = itebfragment->GetSNFragment();
      if (itebfragment->GetFragmentID() == 0) {
	snref = sn;
      } else {
	if (snref != sn) {
	  printf("SN mismatch!!!! (RefS/N:%d, S/N[%d]:%d)\n", snref, itebfragment->GetFragmentID(), sn);
	  std::vector<EBFragment>::iterator iitebfragment;   //!< Main thread iterator
	  for (iitebfragment = ebfragment.begin(); iitebfragment != ebfragment.end(); ++iitebfragment) {
	    if (iitebfragment->GetEnable()) {
	      iitebfragment->PrintSome();
	      
	      
	    }
	  }
	}				
      }
      
      itebfragment->AddBanksToEvent(pevent);
    }
  }
  
  INT ev_size = bk_size(pevent);
  if(ev_size == 0) {
    cm_msg(MINFO,"read_trigger_event", "******** Event size is 0, SN: %d", sn);
  }
  
  return ev_size;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Interrupt configuration (not implemented)
 *
 * Routine for interrupt configuration if equipment is set in EQ_INTERRUPT
 * mode.  Not implemented right now, returns SUCCESS.
 *
 * \param   [in]  cmd Command for interrupt events (see midas.h)
 * \param   [in]  source Equipment index number
 * \param   [in]  adr Interrupt routine (see mfe.c)
 *
 * \return  Midas status code
 */
extern "C" INT interrupt_configure(INT cmd, INT source, POINTER_T adr)
{
  switch (cmd) {
  case CMD_INTERRUPT_ENABLE:
    break;
  case CMD_INTERRUPT_DISABLE:
    break;
  case CMD_INTERRUPT_ATTACH:
    break;
  case CMD_INTERRUPT_DETACH:
    break;
  }
  return SUCCESS;
}

//---------------------------------------------------------------------------------
/**
 * \brief   Event Assembly and "Readout"
 *
 * Event readout routine.  In this case (event builder)
 * Go through all the enabled ring buffer for extracting the event and composing the
 * final event (going to SYSTEM)
 * The Final event is composed of:
 * DTM bank & a selected set of banks from the V1720, V1740, VETO events.
 * The selection is based on the DTM filter result (FpResult()).
 *
 * 1) Get the DTM time stamp as reference
 * 2) Get the time stamp from the rb "Extra info" and compare them to the DTM TS.
 *    should be within some time window. If not abort run.
 * 3) Extract the rb "Extra Q-histo info" and cummulate an overall Q-histo composed
 *    of all the 4 V1720 Q-histos (one per V1720 fragment)
 * 5) Compose the final event
 * 6) send it to SYSTEM
 *
 * \param   [in]  pevent Pointer to event buffer
 * \param   [in]  off Caller info (unused here), see mfe.c
 *
 * \return  Size of the event
 */
INT TSDeapAssembly(char *pevent, INT off)
{
  
  if (!runInProgress) return 0;
  
  sn = SERIAL_NUMBER(pevent);
  
  // Prepare event for MIDAS bank
  bk_init32(pevent);
  
  /* loop over the involved fragment for that DTM trigger mask
   * read an event (Midas event composed of (ZLE+QT) + 2DWORD(Tsmin,Tsmax) + Q-histo
   * Keep the pevent pointer to the event fragment
   */
  
  // Check for data in DTM fragment (first fragment)
  itebfragment = ebfragment.begin();
  
  // Grab the output mask from DTM bank.
  std::pair<unsigned int,unsigned int> result = itebfragment->GetDTMTriggerMaskUsed();
  unsigned int triggerMaskUsed = result.first;
  unsigned int dtmTimestamp = result.second;
  
  // Q vs T histogram, summed over all boards.
  std::vector<int> Qhisto;
  // Same, but not charge-weighted
  std::vector<int> Nhisto;
  
  // Loop over other fragments.
  for(unsigned int ifrag = 1; ifrag < ebfragment.size(); ifrag++){
    
    // If the fragment is disabled, then ignore
    if (!ebfragment[ifrag].GetEnable()){continue;}
    
    // If the fragment is not requested by DTM bank, then ignore
    if(!(ebfragment[ifrag].GetDtmTmask() & triggerMaskUsed)){ continue; }
    
    // Function will check the timestamp for fragment. 
    // It will also add to the Q vs T histogram, if this is V1720 fragment.
    bool bTSMatch = ebfragment[ifrag].CheckTSAndGetQT(0, Qhisto, Nhisto, dtmTimestamp);

    if (!bTSMatch){
			timestampErrorWarning = true;
			
			if(fStrictTimestampMatching) { // Only do anything if we asked for strict timestamp checks.	 			
			
				// Make sure that we only call cm_transition once... multiple calls seems to confuse things.
				if(!eor_transition_called){
					cm_msg(MERROR, "Assembly", "Failure in timestamp check and assembly of fragment %s; stopping run.",ebfragment[ifrag].GetEqpName().c_str());
					cm_transition(TR_STOP, 0, NULL, 0, TR_DETACH, 0);
					eor_transition_called = true;
				}
			}
    }
  }


  // now add fragments to event, based on result of trigger decision
  for (itebfragment = ebfragment.begin(); itebfragment != ebfragment.end(); ++itebfragment) {

    // If the fragment is disabled, then ignore
    if (!itebfragment->GetEnable()){continue;}

    if(itebfragment == ebfragment.begin()) {
      // DTM bank is first and always saved
      itebfragment->AddBanksToEvent(pevent);
    } else{
      if(!(itebfragment->GetDtmTmask() & triggerMaskUsed)) {
        // If the fragment is not requested by DTM bank, then ignore
        continue;
      }

      itebfragment->AddBanksToEvent(pevent);
    }
  }

  
  if (sn % 500 == 0) printf(".");
  
  INT ev_size2 = bk_size(pevent);
  if(ev_size2 == 0)
    cm_msg(MINFO,"read_trigger_event", "******** Event size is 0, SN: %d", sn);

  return ev_size2;
  
}

//---------------------------------------------------------------------------------

// Variable to keep track of which ring buffers are above
// 70% threshold.
int rb_is_above_threshold[NBFRAGMENT] = {0};

INT read_buffer_level(char *pevent, INT off) {
  
  bk_init32(pevent);
  
  uint32_t *pdata;
  
  // Event Builder levels. Save the number of events currently in each ring buffer.
  char bankName[5] = "EBLV"; 
  bk_create(pevent, bankName, TID_DWORD, (void **) &pdata);
  
  // Want a fixed length for the bank.  
  for(unsigned int i = 0; i < 10; i++){
    if(i < ebfragment.size() && ebfragment[i].IsEnabled())
      *pdata++ = ebfragment[i].GetNumEventsInRB();
    else
      *pdata++ = 0;
  }
  bk_close(pevent, pdata); 
  
  // Make bank with the fraction of each event builder ring buffer that is filled.
  char bankName2[5] = "EBFR"; 
  double *pdata2;
  int rb_level;
  bk_create(pevent, bankName2, TID_DOUBLE, (void **) &pdata2);

  // Want a fixed length for the bank.  
  for(unsigned int i = 0; i < 10; i++){
    if(i < ebfragment.size() && ebfragment[i].IsEnabled()){			
			rb_get_buffer_level(ebfragment[i].GetRingBufferHandle(), &rb_level);

			double fill_frac = 100.0 * rb_level/(float)event_buffer_size;
			*pdata2++ = fill_frac;

			// Also, print warnings if the fraction is above 70%.
			// Only print warnings when fraction first goes above.
			if(fill_frac > 70.0){
				if(!rb_is_above_threshold[i]){
					rb_is_above_threshold[i] = 1;
					cm_msg(MINFO, "read_buffer_level", "The EB ring buffer for fragment %s (ID=%i) is more than 70per full (%f full).  Probably indicates either spurious events or that the mlogger cannot write data to disk quickly enough."
								 ,ebfragment[i].GetBufferName().c_str(),ebfragment[i].GetFragmentID(), fill_frac);
				}
			}else{
				rb_is_above_threshold[i] = 0;
			}
			

		}else
      *pdata2++ = 0;
  }
  bk_close(pevent, pdata2); 


  return bk_size(pevent);
}
