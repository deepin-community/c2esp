/* 
 *   Kodak ESP Cxxx (OPL?) Control Language filters for the  Common UNIX
 *   Printing System (CUPS).
 *  common functions for c2esp, c2espC filters
 *
 *  copyright Paul Newall May 2010 - Jan 2014. VERSION 4 (first used in c2esp26) 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "../config.h"
#include <stdio.h>
#include <string.h>
#include <cups/sidechannel.h> //FlushBackChannel, and the side channel functions and constants
#include <fcntl.h> //files
#include <sys/stat.h> //for chmod
#include <time.h> //time functions used for debugging

#if HAVE_CUPSFILTERS_DRIVER_H == 1
#include <cupsfilters/driver.h> //has the dither functions
#else
#include <cups/driver.h> //has the dither functions
#endif

#include "c2espcommon.h" //the common library

/*
 * Constants...
 */
//unsigned char NL = 10;

/*
 * Globals...
 */
char		CallerName[50];  	/* String that identifies the calling program */
int		DoBack;			/* Enables the back channel comms */ 
char 		BackBuf[32000]; //for the back channel replies from the printer
int 		BackBufLen=sizeof(BackBuf)-1;
FILE 		*LogFile = NULL; //file descriptor for log file
time_t		StartTime;
int		BlackPercent, ColourPercent;

time_t KeepAwake(time_t Start, int Interval, FILE *PrintFile)
{
// Keeps the printer connection awake by sending DeviceStatus query not sooner than the specified interval in seconds
// Usage:   Start = KeepAwake(Start, Interval);
	if(time(NULL) - Start > Interval)
	{
		DoLog("Keeping printer awake by DeviceStatus?\n",0,0);
		GoodExchange(PrintFile, "DeviceStatus?", "0101,DeviceStatus.ImageDevice", DoBack,   1,  1.0);
		return (time(NULL));
	}
	else return (Start);
}


void SetupLogging(char *ExtCallerName, int ExtDoBack, char *ExtLogFileName)
{
	strcpy(CallerName,ExtCallerName);
	DoBack=ExtDoBack;
	if(strlen(ExtLogFileName)>0)
	{
		remove(ExtLogFileName); //to be sure I only see the latest
		LogFile = fopen(ExtLogFileName, "w"); //open the log file
		sleep(3); //does this help chmod to work?
		chmod(ExtLogFileName, S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
  		setbuf(LogFile, NULL);
		fprintf(LogFile, "KodakPrintLog %s\n",ExtCallerName);
	}
	StartTime = time(NULL);
}

void CloseLogging()
{
	if(LogFile) 
	{
		DoLog("Closing log\n",0,0);
		fclose(LogFile);
	}
}

void DoLog(char *PrintFormat, int I1, int I2)
{
	//prints a line with 2 integers to the log file and the cups error log
	char CupsFormat[200]; 
	strcpy(CupsFormat, "DEBUG: ");
	strcat(CupsFormat,CallerName);
	strcat(CupsFormat,":%d : ");
	strncat(CupsFormat,PrintFormat,150); //crop PrintFormat to avoid FAILING WITH BUFFER OVERFLOW
	// add \n if not \n at the end of cupsformat
	if(CupsFormat[strlen(CupsFormat)-1] != NL) strcat(CupsFormat,"\n");
	fprintf(stderr, CupsFormat, time(NULL)-StartTime, I1, I2);
	if (LogFile != NULL) fprintf(LogFile, CupsFormat, time(NULL)-StartTime, I1, I2);
}

void DoLogString(char *PrintFormat, char *String)
{
	//prints a line with a string to the log file and the cups error log
	char CupsFormat[200]; 
	strcpy(CupsFormat, "DEBUG: ");
	strcat(CupsFormat,CallerName);
	strcat(CupsFormat,":%d : ");
	strncat(CupsFormat,PrintFormat,150); //crop PrintFormat to avoid FAILING WITH BUFFER OVERFLOW
	fprintf(stderr, CupsFormat, time(NULL)-StartTime, String);
	if (LogFile != NULL) fprintf(LogFile, CupsFormat, time(NULL)-StartTime, String);
}

/* DoOutJob used to enable one call to send to the specified job file and to stdout (if not testing)
And log the result */
void DoOutJob(FILE *OutFile, char *PrintFormat, int I1, int I2)
{
	int BytesRead = 0; //int because cupsBackChannel can return -1
	char Display[80];
	char LogFormat[200];
	int i;

	if (OutFile) fprintf(OutFile, PrintFormat, I1, I2); //to the specified file
#if TESTING == 0
	strcpy(LogFormat, "-> ");
#else
	strcpy(LogFormat, "-block- ");
#endif
	strcat(LogFormat,PrintFormat);
	DoLog(LogFormat, I1, I2); //and the log
#if TESTING == 0
	fprintf(stdout, PrintFormat, I1, I2); //and to the output
	fflush(stdout);

	if(DoBack)
	{
		BytesRead = cupsBackChannelRead(BackBuf, BackBufLen, 0.5); //read the reply from printer
		if(BytesRead >= 1) 
		{
			if(BytesRead<BackBufLen) BackBuf[BytesRead]=0; //add null terminator NB BytesRead==-1 if nothing read
			for(i=0;i<79;++i) Display[i] = BackBuf[i]; //copy the first 79 chars to Display
			Display[79] = 0; //add null terminator
			DoLogString("Reply = %s\n", Display);
		}
		else DoLog("No reply\n", 0,0);
	}

#endif
//	KeepAwakeStart = time(NULL); // reset timer
}


/* FlushBackChannel gets rid of any previous reply that could cause confusion */
int FlushBackChannel(char *IdString, float DrainTime)
{
//returns 1 if sucessful
	cups_sc_status_t status;
	char BackBuf[2]; //useless buffer to satisfy cupsSideChannelDoRequest
	int BackBufLen=sizeof(BackBuf)-1;
	if(DoBack)
	{
		status = cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT, BackBuf, &BackBufLen, DrainTime);
		if(status == CUPS_SC_STATUS_OK) 
		{
			DoLogString("<did DRAIN_OUTPUT %s>\n", IdString);
			return(1);
		}
		else
		{
			if(status == CUPS_SC_STATUS_TIMEOUT) DoLogString("<Failed DRAIN_OUTPUT %s = Timeout>\n", IdString);
			else if(status == CUPS_SC_STATUS_IO_ERROR)  DoLogString("<Failed DRAIN_OUTPUT %s = IO error>\n", IdString);
			else if(status == CUPS_SC_STATUS_NOT_IMPLEMENTED) DoLogString("<Failed DRAIN_OUTPUT %s = not implemented>\n", IdString);
			else  DoLogString("<Failed DRAIN_OUTPUT %s = unknown reason>\n", IdString);
			return(0);
		}
	}
	else return(0);
}


/* GoodExchange now matches against substrings in the reply so we can cope with queued messages better. Thanks to Gordon for this improvement to GoodExchange 4/11/11
	The UnexpectedLogLimit added 11/11/11
	Note that strtok() replaces the delimiters by null bytes, so you can't search the buffer easily afterwards.
	It returns the nubmer of bytes read if the reply includes the one expected,
	otherwise -(the number of bytes read) if the reply did not include Expect, or 0 if there was no reply */


int GoodExchange(FILE *PrintFile, char *Command, char *Expect, int DoBack,  unsigned int SleepTime, float ReplyTime)
{
	int BytesRead = 0; //int because cupsBackChannel can return -1
	char Display[80];
	int i;
	int UnexpectedCount = 0;
	const int UnexpectedLogLimit = 5; //stops the log file being filled with Status replies due to keep awake.
	char * Token1;
	char * TokenList;
	const char * Delimiters = ";&";         // ; for normal replies & for device.status? requests....
                                                // don't actually need the info but nice to be able to read it all.....
	int ReturnSign = -1;                    //assume we won't find the string we want 
	int BlackPercentFound, ColourPercentFound;
#if TESTING == 0
	DoLogString("-> %s\n", Command); //now also sends to stderr
#else
	DoLogString("-block- %s\n", Command); //now also sends to stderr
#endif
	if(PrintFile) fprintf(PrintFile, "%s", Command); //to the global print file
#if TESTING == 0
	fprintf(stdout, "%s", Command); //printer command
	fflush(stdout); //force a packet to the printer so it can reply
	sleep(SleepTime); //give it a chance to reply before trying to read the reply (may not be needed)

	if(DoBack)
	{
		BytesRead = cupsBackChannelRead(BackBuf, BackBufLen, ReplyTime); //read the reply from printer
		if(BytesRead < 1) 
		{
			DoLog("No reply\n",0,0);
			return 0;
		}
		else
		{
			BackBuf[BytesRead]=0; //add null terminator NB BytesRead==-1 if nothing read
			//fprintf(stderr,"thisline->%s\n",BackBuf);

			// search for any special replies here before strtok changes the buffer
			BlackPercentFound=MarkerPercent(BackBuf,0);
			ColourPercentFound=MarkerPercent(BackBuf,1);
			if(BlackPercentFound >= 0) BlackPercent=BlackPercentFound;
			if(ColourPercentFound >= 0) ColourPercent=ColourPercentFound;

			TokenList = BackBuf;
			Token1 = strtok(TokenList , Delimiters);		
			while (Token1 != NULL)
				{
				for(i=0;i<79;++i) Display[i] = Token1 [i]; //copy the first 79 chars to Display
		                if (strncmp(Token1 , Expect, strlen(Expect) -1) == 0) //reduce string length by 1 as ; removed 
					{
					DoLogString("Expected reply = %s\n", Display);
					ReturnSign = 1;
			 		}
				else
					{
					// limit the number of unexpected replies that are logged
					if(UnexpectedCount <= UnexpectedLogLimit) DoLogString("Unexpected reply = %s\n", Display);
					++UnexpectedCount;
					//ReturnSign defaults to unexpected unless changed by a single occurance of expected...
					//so don't alter it here!
                                        }
				Token1 = strtok(NULL , Delimiters);	
				} 
			return (ReturnSign * BytesRead);
		}
	}
	return 0;
#endif
}

int
MarkerPercent(char *Buf, int GetColour) /* GetColour = 1 for "Color" or 0 for "Black" */
{
	/* search for the ink data in the buffer. Returns -1 if not found */
	char *MarkerLevelString;
	
		if(GetColour) MarkerLevelString = strstr(Buf, "DeviceStatus.Printer.InkLevelPercent.Color=");
		else MarkerLevelString = strstr(Buf, "DeviceStatus.Printer.InkLevelPercent.Black=");
		if (MarkerLevelString)
		{
			//DoLog("Found marker level",0,0);
			MarkerLevelString = strstr(MarkerLevelString, "=");
			if (MarkerLevelString)
			{
				if(strncmp(MarkerLevelString + 1,"F",1)==0) 
				{
					DoLog("Found marker %d level Full = %d",GetColour,100);
					return (100);
				}
				else 
				{
					DoLog("Found marker %d level %d",GetColour, atoi(MarkerLevelString + 1));
					return (atoi(MarkerLevelString + 1));
				}
			}
		}
		else 
		{
			//DoLog("Failed to find marker %d level",GetColour,0);
			return -1;
		}
	return -1;
}


void
MarkerSetup()
{
   	fprintf(stderr, "ATTR: marker-colors=black,magenta\n"); //displays ink drops in printer manager
   	fprintf(stderr, "ATTR: marker-names=black,colour\n");
}



void SetPaperSize(char Size[], int PaperPoints)
{
    //converts length of page in cups header (in points) into a string that the printer recognises

	strcpy(Size, "MediaSize=na_letter_8.5x11in;"); //default

    switch (PaperPoints)
    {
      case 421 : // A6 
		strcpy(Size, "MediaSize=iso_a6_105x148mm;");
	  break;
      case 432 : // Photo 4x6" 
		strcpy(Size, "MediaSize=na_index4x6_4x6in;");
	  break;
      case 504 : // Photo 5x7" 
		strcpy(Size, "MediaSize=na_5x7_5x7in;");
	  break;
      case 540 : // Monarch Envelope 
	  break;
      case 595 : // A5 
		strcpy(Size, "MediaSize=iso_a5_148x210mm;");
	  break;
      case 624 : // DL Envelope 
		strcpy(Size, "MediaSize=iso_dl_110x220mm;");
	  break;
      case 649 : // EnvC5 Envelope 
		strcpy(Size, "MediaSize=iso_c5_162x229mm;");
	  break;
      case 684 : // Env10 Envelope 
		strcpy(Size, "MediaSize=na_number10_4.125x9.5in;");
	  break;
      case 709 : // B5 Envelope 
		strcpy(Size, "MediaSize=iso_b5_176x250mm;");
	  break;
      case 720 : // Photo 8x10" 
		strcpy(Size, "MediaSize=na_govtletter_8x10in;");
	  break;
      case 756 : // Executive
		strcpy(Size, "MediaSize=na_executive_7.25x10.5in;");
	  break;
      case 792 : // Letter 
		strcpy(Size, "MediaSize=na_letter_8.5x11in;");
 	  break;
      case 842 : // A4 
		strcpy(Size, "MediaSize=iso_a4_210x297mm;");
	  break;
      case 1008 : // Legal
		strcpy(Size, "MediaSize=na_legal_8.5x14in;");
	  break;
      case 1191 : // A3 
	  break;
      case 1224 : // Tabloid 
	  break;
    }
}

void
DisplayHeader(cups_page_header2_t *header)
{
 /*
  * Show page device dictionary...
  */

  DoLog("StartPage...\n",0,0);
  DoLogString("MediaClass = \"%s\"\n", header->MediaClass);
  DoLogString("MediaColor = \"%s\"\n", header->MediaColor);
  DoLogString("MediaType = \"%s\"\n", header->MediaType);
  DoLogString("OutputType = \"%s\"\n", header->OutputType);
  DoLog("AdvanceDistance = %d\n", header->AdvanceDistance,0);
  DoLog("AdvanceMedia = %d\n", header->AdvanceMedia,0);
  DoLog("Collate = %d\n", header->Collate,0);
  DoLog("CutMedia = %d\n", header->CutMedia,0);
  DoLog("Duplex = %d\n", header->Duplex,0);
  DoLog("HWResolution = [ %d %d ]\n", header->HWResolution[0], header->HWResolution[1]);
  DoLog("ImagingBoundingBox = [ %d %d", header->ImagingBoundingBox[0], header->ImagingBoundingBox[1]);
  DoLog(" %d %d ]\n", header->ImagingBoundingBox[2], header->ImagingBoundingBox[3]);
  DoLog("InsertSheet = %d\n", header->InsertSheet,0);
  DoLog("Jog = %d\n", header->Jog,0);
  DoLog("LeadingEdge = %d\n", header->LeadingEdge,0);
  DoLog("Margins = [ %d %d ]\n", header->Margins[0], header->Margins[1]);
  DoLog("ManualFeed = %d\n", header->ManualFeed,0);
  DoLog("MediaPosition = %d\n", header->MediaPosition,0);
  DoLog("MediaWeight = %d\n", header->MediaWeight,0);
  DoLog("MirrorPrint = %d\n", header->MirrorPrint,0);
  DoLog("NegativePrint = %d\n", header->NegativePrint,0);
  DoLog("NumCopies = %d\n", header->NumCopies,0);
  DoLog("Orientation = %d\n", header->Orientation,0);
  DoLog("OutputFaceUp = %d\n", header->OutputFaceUp,0);
  DoLog("PageSize = [ %d %d ]\n", header->PageSize[0], header->PageSize[1]);
  DoLog("Separations = %d\n", header->Separations,0);
  DoLog("TraySwitch = %d\n", header->TraySwitch,0);
  DoLog("Tumble = %d\n", header->Tumble,0);
  DoLog("cupsWidth = %d\n", header->cupsWidth,0);
  DoLog("cupsHeight = %d\n", header->cupsHeight,0);
  DoLog("cupsMediaType = %d\n", header->cupsMediaType,0);
  DoLog("cupsBitsPerColor = %d\n", header->cupsBitsPerColor,0);
  DoLog("cupsBitsPerPixel = %d\n", header->cupsBitsPerPixel,0);
  DoLog("cupsBytesPerLine = %d\n", header->cupsBytesPerLine,0);
  DoLog("cupsColorOrder = %d\n", header->cupsColorOrder,0);
  DoLog("cupsColorSpace = %d\n", header->cupsColorSpace,0);
  DoLog("cupsCompression = %d\n", header->cupsCompression,0);
}

int 
HeaderInvalid(cups_page_header2_t *header)
{
/* checks the header has sensible values and returns 1 if they are not sensible */

	if(header->HWResolution[0] != 300 && header->HWResolution[0] != 600) 
	{
		DoLog("Header error:  x resolution %d\n",header->HWResolution[0],0);
		return 1;
	}
	return 0;
}

