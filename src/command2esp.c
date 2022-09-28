/*
 *
 *   Kodak ESP command filter for CUPS.
 *
 *   Copyright 2011 - Sept 2012 by P.Newall.
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
 * Contents:
 *
 *   main() - Main entry and command processing.
 */

/* filter must implement the following:
Exchange for test page
Maintenance=002?
0096, OK, Maintenance Started;

Exchange for alignment
Maintenance=006?
0096, OK, Maintenance Started;

Exchange for clean
Maintenance=003?
0096, OK, Maintenance Started;
*/

/*#define DEBUGFILES 0  DEBUGFILES 1 creates files in /tmp to help debug */
/*#define TESTING 0  TESTING 1 suppresses output to the printer to help debug */

/*
 * Include necessary headers...
 */

#include "../config.h"
#include <cups/cups.h>
#include <cups/sidechannel.h> //FlushBackChannel, and the side channel functions and constants
#include "string.h"
#include <ctype.h> 
#include <fcntl.h> //files
#include <time.h> //time functions used for debugging
#include <sys/stat.h> //chmod

#if HAVE_CUPSFILTERS_DRIVER_H == 1
#include <cupsfilters/driver.h> //has the dither functions
#else
#include <cups/driver.h> //has the dither functions
#endif

#include "c2espcommon.h" //the common library

time_t		StartTime;
//FILE 		*LogFile = NULL; //file descriptor for log file
FILE 		*PrintFile = NULL; //file descriptor for debug file
//char 		BackBuf[32000]; //for the back channel replies from the printer
//int 		BackBufLen=sizeof(BackBuf)-1,
int		DoBack=1;			/* Enables the back channel comms */ 

/*
time_t KeepAwake(time_t Start, int Interval)
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
*/

void
KeepAwakeFor(int Duration, int Interval)
{
/* Keep the printer connection awake for Duration seconds, doing KeepAwake every Interval seconds */
	time_t 	KeepAwakeStart;
	int	i;

	KeepAwakeStart = time(NULL);
	for (i=0;i<Duration;++i)
	{
		sleep(1);
		KeepAwakeStart = KeepAwake(KeepAwakeStart, Interval, PrintFile); 
	}
}

/*
 * 'main()' - Main entry and processing of driver.
 */

int						/* O - Exit status */
main(int  argc,					/* I - Number of command-line arguments */
     char *argv[])				/* I - Command-line arguments */
{
  FILE		*fp;				/* Command file */
  char		line[1024],			/* Line from file */
		*lineptr;			/* Pointer into line */
	int StatusLength;

    	fputs("INFO: command2esp running\n", stderr);
	StartTime = time(NULL);

#if DEBUGFILES == 1
	SetupLogging("c2espCommand",DoBack,"/tmp/KodakCommandLog");
#else
	SetupLogging("c2espCommand",DoBack,"");
#endif

 /*
  * Check for valid arguments...
  */

  if (argc < 6 || argc > 7)
  {
   /*
    * We don't have the correct number of arguments; write an error message
    * and return.
    */

    fputs("ERROR: command2esp job-id user title copies options [file]\n", stderr);
    return (1);
  }


 /*
  * Open the command file as needed...
  */

  if (argc == 7)
  {
    if ((fp = fopen(argv[6], "r")) == NULL)
    {
      perror("ERROR: Unable to open command file - ");
      return (1);
    }
  }
  else
    fp = stdin;

 /*
  * Read the commands from the file and send the appropriate commands...
  */

  while (fgets(line, sizeof(line), fp) != NULL)
  {
   /*
    * Drop trailing newline...
    */

    lineptr = line + strlen(line) - 1;
    if (*lineptr == '\n')
      *lineptr = '\0';

   /*
    * Skip leading whitespace...
    */

    for (lineptr = line; isspace(*lineptr); lineptr ++);  /* isspace is in ctype.h */
   /*
    * Skip comments and blank lines...
    */

    if (*lineptr == '#' || !*lineptr)
      continue;

   /*
    * Parse the command...
    */

    if (strncasecmp(lineptr, "Clean", 5) == 0)
    {
     /* Clean heads...*/
    	fputs("INFO: command2esp Clean print head\n", stderr);
	DoLog("Clean print head\n",0,0);
	GoodExchange(PrintFile, "Maintenance=003?", "0096, OK, Maintenance Started;", DoBack, 1,  1.0);
	KeepAwakeFor(80,10);      
    }

    else if (strncasecmp(lineptr, "PrintAlignmentPage", 18) == 0)
    {
     /* Print alignment page...*/

    	fputs("INFO: command2esp Print alignment page\n", stderr);
	DoLog("Print alignment page\n",0,0);
	GoodExchange(PrintFile, "Maintenance=006?", "0096, OK, Maintenance Started;", DoBack,   1,  1.0);
	KeepAwakeFor(80,10);
    } 

    else if (strncasecmp(lineptr, "PrintSelfTestPage", 17) == 0)
    {
    	fputs("INFO: command2esp Print Self Test Page\n", stderr);
	DoLog("Print Self Test Page\n",0,0);
	GoodExchange(PrintFile, "Maintenance=002?", "0096, OK, Maintenance Started;", DoBack,   1,  1.0);
	// Hero 9.1 does not need to be kept awake. ESP 5250 does?
	//sleep(80); //did not work for ESP 5250
	KeepAwakeFor(80,15);
    }

    else if (strncasecmp(lineptr, "ReportLevels", 12) == 0)
    {
     /* Report ink levels... */

	StatusLength=abs(GoodExchange(PrintFile, "DeviceStatus?", "0101,DeviceStatus.ImageDevice", DoBack,  1,  1.0));
	DoLog("StatusLength=%d\n",StatusLength,0);
/* you can get unexpected reply if there is an ink low warning then GoodExchange will be -ve */
/* aquire ink levels here? DeviceStatus.Printer.InkLevelPercent.Colour=nn%&DeviceStatus.Printer.InkLevelPercent.Black=nn% */
	if(StatusLength>0)
	{
/*		GoodExchange will have found the marker levels and set the globals BlackPercent and ColourPercent
*/
 		MarkerSetup();
		// set the levels displayed in system-config-printer
   		fprintf(stderr,"ATTR: marker-levels=%d,%d\n",BlackPercent,ColourPercent); 
	}
    }

    else if (strncasecmp(lineptr, "SetAlignment", 12) == 0)
    {
     /*
      * Set head alignment... may not be possible for ESP printers - do nothing
      */
    }

    else
      fprintf(stderr, "ERROR: Invalid printer command \"%s\"!\n", lineptr);
  }

 /*
  * Close the files and return...
  */
  if (fp != stdin) fclose(fp);
  CloseLogging();
  return (0);
}


