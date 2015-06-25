/** Biosemi acqusition tool to stream (downsampled) data to a FieldTrip buffer,
  and write full-rate data to one or multiple GDF files.
  (C) 2010 S. Klanke
*/
#include <stdio.h>
#include <time.h>
#include <BioSemiClient.h>
#include <OnlineDataManager.h>
#include <ConsoleInput.h>
#include <StringServer.h>

StringServer ctrlServ;
ConsoleInput conIn;
char hostname[256];
int port, ctrlPort;
char gdfname[1024];
char cfgname[1024];
bool haveGDF;

//----------------------------------------------------------------------------------------
void acquisition(BioSemiClient& BS, int numHwChan, int fSample) {
  // number of samples grabbed from USB in total
  int sampleCounter = 0;
  // value of the trigger channel (middle 16 bits of that)
  int ttlTriggerState = 0, triggerState = 0;
  // Previous sample index
  int sOLD = 0;
  // "CMS in range" flag as reported most recently
  int cmsInRange = 0;
  // number of samples until we're due a report again
  int nsBattery = 0, nsCMS = 0;
  // ODM grabs original 32-bit integer data, scales it to microVolts for streaming
  OnlineDataManager<int, float> ODM(1, numHwChan, (float) fSample);
  // just for reporting: print current time relative to time when we started this
  double T0 = BS.getCurrentTime();
  int stimulusCount = 0;

  if (ODM.configureFromFile(cfgname) != 0) {
    fprintf(stderr, "Configuration file is invalid\n");
    return;
  }

  if (!strcmp(hostname, "-")) {
    if (!ODM.useOwnServer(port)) {
      fprintf(stderr, "Could not spawn buffer server on port %d.\n",port);
      return;
    }
  } else {
    bool givennotice = false;
    while (!ODM.connectToServer(hostname, port)) {
      if (!givennotice) {
        fprintf(stderr, "Waiting for buffer server at %s:%d.\n",hostname, port);
        givennotice = true;
      }
    }
  }
  fprintf(stderr, "CONNECTED TO BUFFER SERVER at %s:%d!\n",hostname, port);


  ODM.setStatusLabel(0, "STATUS");
  // min:  (-2^31)/8192.0  (int32 -> microVolt)
  // max: (2^31-1)/8192.0  (int32 -> microVolt)
  ODM.setPhysicalLimits(-262144.0, 262143.99987792969);
  // scale streamed data to microvolts
  ODM.setPhysicalDimCode(GDF_MICRO + GDF_VOLT);
  ODM.setSlopeAndOffset(1.0/8192.0, 0);

  if (haveGDF) ODM.setFilename(gdfname);

  if (!ODM.enableStreaming()) return;

  if (haveGDF) {
    printf("\nPress <Esc> to quit, <S> to enable saving, <D> to disable saving\n\n");
  } else {
    printf("\nPress <Esc> to quit\n\n");
  }

  //--------------------------------
  // Loop until program termination
  //--------------------------------
  while (1) {
    BioSemiBlock block;
    int *ptr;
    const SignalConfiguration& config = ODM.getSignalConfiguration();
    double batt = 0.0;

    if (conIn.checkKey()) {
      int c = conIn.getKey();
      if (c==27) break; // quit

      if (haveGDF) {
        if (c=='s' || c=='S') {
          if (!ODM.enableSaving()) {
            fprintf(stderr, "Cannot enable saving to GDF\n");
            break;
          }
        }

        if (c=='d' || c=='D') {
          ODM.disableSaving();
        }
      }
    }
    ctrlServ.checkRequests(ODM);

    if (!BS.checkNewBlock(block)) {
      BS.msleep(1);
      continue;
    }

    if (numHwChan >= 280) {
      batt = BS.getValue(block.startIndex + 279);
      batt=(batt/2097152)-175.5;
      if (batt < 0) batt = 0;
      if (batt > 100) batt = 100;
    }

    printf("T=%8.3f Ptr=%8i, samples=%3i, batt=%5.1f%%\r", BS.getCurrentTime() - T0, block.startIndex, block.numSamples, batt);
    if (block.numSamples != block.numInSync) {
      fprintf(stderr, "USB device out of sync (%i / %i) -- exiting\n", block.numInSync, block.numSamples);
      break;
    }

    ptr = ODM.provideBlock(block.numSamples);
    if (ptr == NULL) {
      fprintf(stderr, "Out of memory\n");
      break;
    }

    if (nsBattery >= 0) {
      if (nsBattery<block.numSamples) {
        // we're due a report of the battery level
        ODM.getEventList().add(0, "BATTERY", (float) batt);
        //printf("\n-!- Battery level = %4.1f\n", batt);
        // next report in getBatteryRefresh() seconds
        nsBattery = config.getBatteryRefresh() * fSample;
        // if this is zero, user doesn't want events,
        // so prevent further output by setting it to -1
        if (nsBattery == 0) nsBattery = -1;
      } else {
        nsBattery -= block.numSamples;
      }
    }

    for (int j=0;j<block.numSamples;j++) {
      // dest_j points to j-th sample in block provided by ODM
      int *dest_j = ptr + j*(1+numHwChan);
      // source offset, j-th sample
      int soff   = block.startIndex + j*block.stride;
      // status word (incl. triggers)
      int value  = BS.getValue(soff + 1);
      // "CMS within range" flag in Biosemi logic
      int cmsBit = (value & 0x10000000) ? 0:1;

      // copy status value
      dest_j[0] = value;

      if (nsCMS == 0 || cmsBit != cmsInRange) {
        ODM.getEventList().add(j, "CMS_IN_RANGE", cmsBit);
        //printf("\n-!- CMS in range: %i\n", cmsBit);
        cmsInRange = cmsBit;
        nsCMS = config.getStatusRefresh() * fSample;
      } else {
        --nsCMS;
      }


      // The NIDAQ uses TTL channel 4 to send the joystick events.
	  //This is in the second half of the first byte of the 24-bit Biosemi status channel
      //(there is an extra byte that is appended to the LSB of the 24-bit number to make it 32-bits long)
	  //so shift 8 to get rid of the extra byte, and another 2 to get rid of the TTL channels reserved for the sync bit
      int ttl_value = (value & 0x00000F00) >> 10;

      // Photodiode values are found in the first byte of the 24-bit BioSemi status channel
      value = (value & 0x0000FF00) >> 8;

      // Update Update the value of the current sample (i.e., time at the BioSemi fs = 2048 Hz)
      int sNow = sampleCounter + j;

      // Check for events on the first byte (this is where the sync pulse is)
      // The comparison with sNow-sOLD ensures that we aren't double counting stimuli as
      // can happen...
      if (value && value!=triggerState && sNow-sOLD > 100) {
        //sOLD = sNow;
        printf("\n-!- FOUND TRIGGER for sample %i with j %d and value %d => \n", sNow,j,value);
		//printf("\n-!- Check JOYSTICK AGAIN3 for sample %i with j %d and value %d => \n", sNow,j,ttl_value);

		//TURN this loop off, we don't care about photodiode events
        if (value == 1 || value == 4) {
          //values 1,4 correspond to lightsensors 1,3 respectively
          ODM.getEventList().add(j, "STATUS", sNow);
          ODM.getEventList().add(j, "event_id", value);
          stimulusCount++;

          time_t seconds;
          seconds = time(NULL);
          printf("\n-!- Full trigger at sample %i at stimulus %d \n", sNow, value, stimulusCount);
        } //end if value == 1,4
      } //end if value and sNow-sOld

      // Parse the TTL value from the response pad and compare with the previous value (so as to avoid duplicate events)
      if (ttl_value && ttl_value!=ttlTriggerState && sNow-sOLD > 1024) {
		  sOLD = sNow;
          printf("\n-!- FOUND JOYSTICK (latest) for sample %i with j %d and value %d =>", sNow,j,ttl_value);
		  if (ttl_value == 2) {
			  ODM.getEventList().add(j, "STATUS", sNow);
			  ODM.getEventList().add(j, "event_id", ttl_value);
          //stimulusCount++;

          //time_t seconds;
          //seconds = time(NULL);
          //printf("\n-!- Full trigger at sample %i at stimulus %d \n", sNow, value, stimulusCount);
		  }

		  /*TURN THIS OFF, ONLY FOR IF KEEPING TRACK OF SPECIFIC BIT
     // Check individual bit values from the TTL input to determine if a unique button press has occurred (either a different button or the previous button was released and then pressed)
          for (int iBit=0; iBit<8; iBit++) {
              int bitValue = ((ttl_value >> iBit) & 0x00000001);
              int prevBitValue = ((ttlTriggerState >> iBit) & 0x00000001);
              
              if (bitValue && (bitValue != prevBitValue)) {
                  printf("\n-!- LOGGING RESPONSE for sample %i with j %d and value %d => \n\n", sNow,j,iBit + 8 + 1);
                  ODM.getEventList().add(j, "RESPONSE", iBit + 8 + 1); //Response value based on indexing from 1:16
              }
          }
		  */
      }//Bottom of ttl_value check

      // Set the current trigger state corresponding to the photodiodes
      triggerState = value;

      // Set the current trigger state corresponding to the RB-Series Response Pad
      ttlTriggerState = ttl_value;

      // copy continuous channels into memory provided by ODM
      // selection, scaling, streaming + saving will be handled from there
      for (int i=0;i<numHwChan;i++) {
        // a memcpy would be a bit faster, but then we'd need to handle
        // the wrapping-around of the BS ringbuffer here
        dest_j[1+i] = BS.getValue(soff + i + 2);
      }
    } // end for()

    if (!ODM.handleBlock()) {
      fprintf(stderr, "Error in handling this data block - stopping\n");
      break;
    }

    sampleCounter += block.numSamples;
  } // end while(1)
} //end acquisition()
//----------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
  BioSemiClient BS;

  if (argc<2) {
//    printf("Usage: biosemi2ft <config-file> [gdf-file] [hostname=localhost [port=1972 [ctrlPort=8000]]]\n");
//    return 0;
    argv[1] = "config64.txt";
  }

  strncpy(cfgname, argv[1], sizeof(cfgname));
#ifdef WIN32
  if (GetFileAttributes(cfgname)==INVALID_FILE_ATTRIBUTES) {
#else
  if (access(cfgname, R_OK)) {
#endif
    fprintf(stderr, "Cannot open configuration file %s\n", cfgname);
    return 1;
  }

  if (argc>2) {
    strncpy(gdfname, argv[2], sizeof(gdfname));
    haveGDF = strcmp(gdfname, "-") != 0;
  } else {
    strcpy(gdfname, "-");
    haveGDF = false;
  }

  if (argc>3) {
    strncpy(hostname, argv[3], sizeof(hostname));
  } else {
    strcpy(hostname, "localhost");
  }

  if (argc>4) {
    port = atoi(argv[4]);
  } else {
    port = 1972;
  }

  if (argc>5) {
    ctrlPort = atoi(argv[5]);
  } else {
    ctrlPort = 8000;
  }

  if (!ctrlServ.startListening(ctrlPort)) {
    fprintf(stderr, "Cannot listen on port %d for configuration commands\n", ctrlPort);
    return 1;
  }

  if (!BS.openDevice()) return 1;

  printf("Speed mode.........: %i\n", BS.getSpeedMode());
  printf("Number of channels.: %i\n", BS.getNumChannels());
  printf("Number of AIB chns.: %i\n", BS.getNumChanAIB());
  printf("Sampling frequency.: %i Hz\n", BS.getSamplingFreq());

  acquisition(BS, BS.getNumChannels() + BS.getNumChanAIB(), BS.getSamplingFreq());

  BS.closeDevice();
  return 0;
} //end main()
//----------------------------------------------------------------------------------------
