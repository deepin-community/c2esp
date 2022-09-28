/* 
 *
 *   Kodak ESP Cxxx (OPL?) Control Language filters for the  Common UNIX
 *   Printing System (CUPS).
 *  common functions for c2esp, c2espC filters header file
 *
 *  copyright Paul Newall May 2010 - Jan 2014. VERSION 3 (first used in c2esp26) 
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

#include <cups/sidechannel.h> //FlushBackChannel, and the side channel functions and constants
#include <fcntl.h> //files
#include <time.h> //time functions used for debugging

/*
 * Constants...
 */
#define DEBUGFILES 0 /* DEBUGFILES 1 creates files in /tmp to help debug */
#define TESTING 0 /* TESTING 1 suppresses the output to the printer */

#define ESC  255
#define NL  10

/*
 * Globals...
*/
char 		BackBuf[32000]; //for the back channel replies from the printer
int		ColourPercent, BlackPercent; //to store the detected marker levels


time_t KeepAwake(time_t Start, int Interval, FILE *PrintFile);

void SetupLogging(char *ExtCallerName, int ExtDoBack, char *ExtLogFileName);

void CloseLogging();

void DoLog(char *PrintFormat, int I1, int I2);
	//prints a line with 2 integers to the log file and the cups error log

void DoLogString(char *PrintFormat, char *String);
	//prints a line with a string to the log file and the cups error log

/* DoOutJob used to enable one call to send to the specified job file and to stdout (if not testing)
And log the result */
void DoOutJob(FILE *OutFile, char *PrintFormat, int I1, int I2);


/* FlushBackChannel gets rid of any previous reply that could cause confusion */
int FlushBackChannel(char *IdString, float DrainTime);
//returns 1 if sucessful

/* GoodExchange sends a command gets reply from the printer on the back channel and compares it with the expected reply
	It returns the number of bytes read if the reply was the one expected, 
	otherwise -(the number of bytes read) if the reply did not match Expect, or 0 if there was no reply */
int GoodExchange(FILE *PrintFile, char *Command, char *Expect, int DoBack,  unsigned int SleepTime, float ReplyTime);

int MarkerPercent(char *Buf, int GetColour); /* GetColour = 1 for "Color" or 0 for "Black" */

void MarkerSetup();

void SetPaperSize(char Size[], int PaperPoints);
    //converts length of page in cups header (in points) into a string that the printer recognises

void DisplayHeader(cups_page_header2_t *header);
 /*
  * Show page device dictionary in stderr and LogFile
  */

int HeaderInvalid(cups_page_header2_t *header);
/* checks the header has sensible values and returns 1 if they are not sensible */


