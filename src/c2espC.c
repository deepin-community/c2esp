/* 
 *
 *   Kodak ESP Cxxx (OPL?) Control Language filter for the  Common UNIX
 *   Printing System (CUPS).
 *
 *  copyright Paul Newall May 2010 - Jan 2014. VERSION 2.7 (c2esp27) 
 *  SUPPORT FOR ESP Cxxxx and Hero SERIES
 *  patch by user awl29 applied to fix problems with non bi-directional printers, smb shared
 *  data chunk size limit applied
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

 *  Params: job-id user title copies options [file]
 *  options = "noback" disables all calls to the back channel for testing.
 *
 
This filter is based loosely on c2esp
 */

#define MAXDATACHUNK 65511 /* it seems windows smb printer can't handle more than 65K at a time? maybe others too */

// debugfiles and testing are defined in c2espcommon.h because they are used in c2espcommon.c
/* #define DEBUGFILES 0  DEBUGFILES 1 creates files in /tmp to help debug 
Currently a large number of files:
KodakPrintLog = text file showing progress of the filter
RasForComp.pbm or ppm = the raster read from cups raster view with image viewer.
KodakUncompressed = The binary page data before compression.
KodakCompPage = The zlib compressed page data
KodakPrintFile = The data that is sent to the printer */

#define TESTING 0  /* TESTING 1 suppresses the output to the printer. Used in development. */

/* #include "config.h" */
#include <cups/raster.h>
#include <cups/sidechannel.h> //FlushBackChannel, and the side channel functions and constants
#include <fcntl.h> //files
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h> //time functions used for debugging
#include "c2espcommon.h" //the common library of c2esp and c2espC

/* for gzip */
#include <zlib.h>
#define SET_BINARY_MODE(file)
#define CHUNK 16384

/*
 * Constants...
 */
char	*Version = "c2espC27";

/*
 * Globals...
 */
unsigned char	*CupsLineBuffer;	//buffer for one line of the cups raster
unsigned char	*DataLineBuffer;	//buffer for one uncompressed line
char		KodakPaperSize[50];  	/* String that the printer expects for paper size */
int		OutBitsPerPixel,	/* Number of bits per color per pixel for printer*/
		Duplex,			/* Current duplex mode */
		Page,			/* Current page number */
		Canceled,		/* Has the current job been canceled? */
 		DoBack;			/* Enables the back channel comms */ 
int		MemUsed = 0; //tracks memory use
time_t 		TimeStart; //to record the start of a section
FILE 		*PrintFile = NULL; //file descriptor for debug file
FILE 		*UncompressedFile = NULL; //file descriptor for file of uncompressed page data

time_t		StartTime;
time_t 		KeepAwakeStart;

//for zlib
z_stream 	strm;
unsigned char	out[CHUNK]; /*buffer for a compressed line */

#if DEBUGFILES == 1
FILE 		*dfp = NULL; //file descriptor for composite raster file
FILE 		*Cyanfp = NULL; //file descriptor for cyan only raster file
FILE 		*Magentafp = NULL; //file descriptor for magenta only raster file
FILE 		*Yellowfp = NULL; //file descriptor for yellow only raster file
FILE 		*Blackfp = NULL; //file descriptor for black only raster file
FILE 		*RawColourFile = NULL; //file descriptor for input to dither
FILE 		*DitheredColourFile = NULL; //file descriptor for output from dither
#endif

void
SetupPrinter(cups_page_header2_t *header)
{
//gets the printer ready to start the job
	int  i;

	for(i=0; i<4; ++i)
	{
		if(GoodExchange(PrintFile, "LockPrinterWait?", "0002, OK, Locked for printing;", DoBack, 1,  3.0) >= 0) break;
	}

	DoOutJob(PrintFile, "Event=StartOfJob;",0,0); //printer command

	if (DoBack) {
		GoodExchange(PrintFile, "DeviceStatus?", "0101,DeviceStatus.ImageDevice", DoBack, 1,  1.0);
// you can get unexpected reply if there is an ink low warning then GoodExchange will be -ve
//aquire ink levels here? DeviceStatus.Printer.InkLevelPercent.Colour=nn%&DeviceStatus.Printer.InkLevelPercent.Black=nn%
//note & used as separator

		DoLog("ColourPercent=%d\n",ColourPercent,0);
		DoLog("BlackPercent=%d\n",BlackPercent,0);
		if(ColourPercent >= 0 && BlackPercent >= 0)
		{
    		fprintf(stderr,"ATTR: marker-levels=%d,%d\n",BlackPercent,ColourPercent); // sets the levels displayed
		}
	}        
	DoOutJob(PrintFile, KodakPaperSize,0,0);   
	if(header->MediaPosition == 0) DoOutJob(PrintFile, "MediaInputTrayCheck=Main;",0,0);
	else if(header->MediaPosition == 1) DoOutJob(PrintFile, "MediaInputTrayCheck=Photo;",0,0);
	else 
	{
		DoOutJob(PrintFile, "MediaInputTrayCheck=Main;",0,0);
		DoLog("Unknown Input Tray no. %d so used main tray", header->MediaPosition, 0);
	}

    if (DoBack) {        
		GoodExchange(PrintFile, "MediaTypeStatus?", "MediaTypeStatus=custom-media-type-deviceunavailable", DoBack,  1,  1.0);
		GoodExchange(PrintFile, "MediaDetect?", "0098, OK, Media Detect Started;", DoBack, 1,  1.0);
		//do MediaTypeStatus? until some media is found typically takes 12 seconds for Hero 9.1
#if TESTING == 0
		sleep(5);
		for(i=0; i<15; ++i) //normal
#endif
#if TESTING == 1
		for(i=0; i<2; ++i) //short for tests
#endif
		{
			DoLog("MediaTypeStatus? try %d\n", i, 0);
			if(GoodExchange(PrintFile, "MediaTypeStatus?", 
				"MediaTypeStatus=custom-media-type-deviceunavailable", DoBack,  2,  2.0) <= 0) break;
		}
	}
	else {
		GoodExchange(PrintFile, "MediaDetect?", "0098, OK, Media Detect Started;", DoBack, 1,  1.0);
		sleep(12);
	}
}

void
ShutdownPrinter(void)
{
	int i, ret;

	DoOutJob(PrintFile, "Event=EndOfJob;",0,0);
	for(i=0; i<20; ++i) /* fast PC might need lots of tries here for printer to finish, how many is reasonable? */
	{
		/* First few tries will be quick so small jobs finish quickly */
		if(i<5) ret=GoodExchange(PrintFile, "UnlockPrinter?", "0003, OK, Printer unlocked;", DoBack, 1,  5.0);
		/* Then tries will be slower so long jobs can finish */
		else ret=GoodExchange(PrintFile, "UnlockPrinter?", "0003, OK, Printer unlocked;", DoBack, 5,  2.0);
		DoLog("UnlockPrinter? try %d returned %d\n", i, ret);
		if(ret >= 0 || strcmp(BackBuf , "3405, Error, Printer not locked") == 0) break;
		//error string has no terminating ; because it has been tokenised
	}
}

void
SetupJob(cups_page_header2_t *header) //Prepare the printer for printing a job.
{
	DoOutJob(PrintFile, "OutputBin=MainSink;",0,0);
	Duplex = header->Duplex;
	if(Duplex == 0) DoOutJob(PrintFile, "Sides=OneSided;",0,0);
	else  DoOutJob(PrintFile, "Sides=TwoSided;",0,0);
	//DoOutJob(PrintFile, "PageOrder=BackToFront;",0,0); //may be possible to implement this
	//DoOutJob(PrintFile, "Copies=1;",0,0); //may an error response? but is sent by windows
	DoOutJob(PrintFile, "MediaType=custom-media-type-autoselection-0-0-0-0;",0,0);
	DoOutJob(PrintFile, KodakPaperSize,0,0);
}

void
FreeBuffers()
{
	DoLog("Free buffers\n",0,0);
  	free(CupsLineBuffer);
  	free(DataLineBuffer);
	MemUsed = 0;
	DoLog("Buffers freed\n",0,0);
}

void
AllocateBuffers(cups_page_header2_t *header)
{
 // Allocate memory for a page of graphics... 

  	if ((CupsLineBuffer = malloc(header->cupsBytesPerLine)) == NULL) 
  	{
		DoLog("ERROR: Unable to allocate %d bytes for CupsLineBuffer!\n",header->cupsBytesPerLine,0);
    		exit(1);
  	}
	else MemUsed = MemUsed + header->cupsBytesPerLine * 1E-3;
 	if ((DataLineBuffer = malloc(header->cupsBytesPerLine+12)) == NULL) 
  	{
		DoLog("ERROR: Unable to allocate %d bytes for DataLineBuffer!\n",header->cupsBytesPerLine+8,0);
    		exit(1);
  	}
	else MemUsed = MemUsed + (header->cupsBytesPerLine+12) * 1E-3;
	DoLog("Buffers allocated %d kb\n",MemUsed,0);
}

void
StartPrinterPage(cups_page_header2_t *header)
{
	int  ResX, ResY;

	DisplayHeader(header);
	ResX = header->HWResolution[0];
	ResY = header->HWResolution[1];
	DoOutJob(PrintFile, "Event=StartOfPage;",0,0);
	DoOutJob(PrintFile, "Page=%d;",Page,0);
	DoOutJob(PrintFile, "Origin.Top=0.0mm;Origin.Left=0.0mm;",0,0);
//	DoOutJob(PrintFile, "Origin.Top=1.0mm;Origin.Left=1.0mm;",0,0);
	if(ResX==300)
	{
		DoOutJob(PrintFile, "PrintQuality=Draft;",0,0);
	}
	else if(ResX==600)
	{
		DoOutJob(PrintFile, "PrintQuality=Normal;",0,0);
	}
	else if(ResX==1200) // may not be an option
	{
		DoOutJob(PrintFile, "PrintQuality=High;",0,0);
	}

    	if (header->cupsColorSpace == CUPS_CSPACE_CMY)
	{

		DoLog("CUPS_CSPACE_CMY  (%d)\n",header->cupsColorSpace,0);
		DoOutJob(PrintFile, "PrintColorspace=Color;",0,0);
		DoOutJob(PrintFile, "AntiBleedControl=Auto;",0,0);
		DoOutJob(PrintFile, "Resolution=%dx%d;", ResX, ResY);
		DoOutJob(PrintFile, "RasterObject.BitsPerPixel=%d;",OutBitsPerPixel,0);
		DoOutJob(PrintFile, "RasterObject.Colorspace=sRGB;",0,0);
	}
	else if	 (header->cupsColorSpace == CUPS_CSPACE_K)
	{
		DoLog("CUPS_CSPACE_K (%d)\n",header->cupsColorSpace,0);
		DoOutJob(PrintFile, "PrintColorspace=Grayscale;",0,0);
		DoOutJob(PrintFile, "AntiBleedControl=Off;",0,0);
		DoOutJob(PrintFile, "Resolution=%dx%d;", ResX, ResY);
		DoOutJob(PrintFile, "RasterObject.BitsPerPixel=%d;",OutBitsPerPixel,0);
		DoOutJob(PrintFile, "RasterObject.Colorspace=Mono;",0,0);
	}
	else	
	{
		DoLog("CUPS_CSPACE_??  (%d)\n",header->cupsColorSpace,0);
	}

	DoOutJob(PrintFile, "RasterObject.Compression=GZIPTok;",0,0);
    	DoOutJob(PrintFile, "RasterObject.Width=%d;", header->cupsWidth,0);
	DoOutJob(PrintFile, "RasterObject.Height=%d;",  header->cupsHeight,0);
}

void
EndPage(void) //Finish a page of graphics.
{
	DoOutJob(PrintFile, "Event=EndOfPage;",  0,0);
	if(PrintFile) fflush(PrintFile);
	FreeBuffers(); /* Free memory... allocated by AllocateBuffers() */
}

void
CancelJob(int sig)	/* - Cancel the current job... I - Signal */
{
  	(void)sig;
  	DoLog("CancelJob: job cancelled by signal\n",0,0);
	CloseLogging();
  	Canceled = 1;
}


unsigned char Byte0(int In)
{
int Byte0Mask = 0xFF;
return (In & Byte0Mask);
}

unsigned char Byte1(int In)
{
int Byte0Mask = 0xFF;
return ((In>>8) & Byte0Mask);
}

int Composite(unsigned char Buffer[],int Posn)
{
//returns the sum of the 3 colour components corresponding to Posn, assuming chunked colour order
return (Buffer[Posn*3]+Buffer[Posn*3+1]+Buffer[Posn*3+2]);
}

/*
 * 'main()' - Main entry and processing of driver.
 */

int					/* O - Exit status */
main(int  argc,	char *argv[])		/* I - Number of command-line arguments, Command-line arguments */
{
  	int			fd;		/* File descriptor */
 	cups_raster_t		*ras;		/* Raster stream from cups */
  	cups_page_header2_t	header;		/* Page header from cups */
  	int			y, c;		
	int			i;
	int 			PixelsLeft,PixelsRight; //count blank pixels at right and left of raster line
	int			PixelsNonBlank; //in a raster line
	int			DataLength; //length of an uncompressed data line
	unsigned char		MinOut=255, MaxOut=0; //to check the range of the dithered output or raster
	int			CheckCount, ret, have, flush;
	FILE	*CompData; //stores the compressed data of a page
	char RasFileName[100]="";
	int BytesToPrint; 
	int CurrentChunkSize; 

	StartTime = time(NULL);
	KeepAwakeStart = time(NULL);

	#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  	struct sigaction action;		/* Actions for POSIX signals */
	#endif /* HAVE_SIGACTION && !HAVE_SIGSET */

 /*
  * Check command-line...
  */
  	if (argc < 6 || argc > 7) //wrong no of arguments
  	{
    	fprintf(stderr, ("Usage: %s job-id user title copies options [file]\n"), "rastertoek");
   		return (1);
  	}
        
	char data;
	int datalen;
	cups_sc_bidi_t bidi;
	cups_sc_status_t status;
        
	/* Tell cupsSideChannelDoRequest() how big our buffer is... */
	datalen = 1;

	/* Get the bidirectional capabilities, waiting for up to 1 second */
	status = cupsSideChannelDoRequest(CUPS_SC_CMD_GET_BIDI, &data, &datalen, 1.0);

	/* Use the returned value if OK was returned and the length is still 1 */
	if (status == CUPS_SC_STATUS_OK && datalen == 1) {
		bidi = (cups_sc_bidi_t) data;
		DoBack = (bidi == CUPS_SC_BIDI_NOT_SUPPORTED ? 0 : 1);
	} 
	else {
		bidi = CUPS_SC_BIDI_NOT_SUPPORTED;
		DoBack = 0;
	}

#if DEBUGFILES == 1
	SetupLogging("c2espC",DoBack,"/tmp/KodakPrintLog");
#else
	SetupLogging("c2espC",DoBack,"");
#endif

  	setbuf(stderr, NULL);
	fprintf(stderr, ("DEBUG:  ================= %s ===================================\n"),Version); 
	DoLogString("Starting %s\n",Version);
        DoLog("Compiled with DEBUGFILES = %d, TESTING = %d\n", DEBUGFILES, TESTING);
        
	DoLog("Number of command line parameters: %d\n", argc, 0);
	int argi;
	for (argi = 0; argi < argc; argi++) {
		DoLogString("  param: '%s'\n", argv[argi]);
	}
	DoLogString("End of command-line parameters\n", "");

	DoLogString("Bi-di/backchannel support: %s\n", (bidi == CUPS_SC_BIDI_NOT_SUPPORTED ? "false" : "true"));
	DoLog("DoBack value: %d\n", DoBack, 0);
        
	MarkerSetup();

 /*
  * Open the page stream...
  */
  	if (argc == 7)
  	{
		if ((fd = open(argv[6], O_RDONLY)) == -1)
   		{
   			fprintf(stderr, ("ERROR: c2espC: Unable to open raster file - %s\n"), strerror(errno));
   			sleep(1);
   			return (1);
   		}
  	}
  	else    fd = 0;
   	fprintf(stderr, ("DEBUG: c2espC: opening raster\n")); 
  	ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

 /*
  * Register a signal handler to eject the current page if the
  * job is cancelled.
  */
  	Canceled = 0;
	#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  	sigset(SIGTERM, CancelJob);
	#elif defined(HAVE_SIGACTION)
  	memset(&action, 0, sizeof(action));
  	sigemptyset(&action.sa_mask);
  	action.sa_handler = CancelJob;
  	sigaction(SIGTERM, &action, NULL);
	#else
  	signal(SIGTERM, CancelJob);
	#endif /* HAVE_SIGSET */

 /*
  * Initialize the print device...
  */

#if DEBUGFILES == 1
	PrintFile = fopen("/tmp/KodakPrintFile", "w");//open the print file
	sleep(3); //does this help chmod to work?
	chmod("/tmp/KodakPrintFile", S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
#endif

/* read the first header */
	if(cupsRasterReadHeader2(ras, &header))
	{
		DoLog("First page Header read after %d sec\n", time(NULL)-StartTime,0);
		SetPaperSize(KodakPaperSize, header.PageSize[1]);
		SetupPrinter(&header);
		DoLog("Printer should be ready by now\n",0,0);
  /* 
  * Process pages as needed...
  */
  		Page = 0;
		do //start of loop for each page
  		{
			DoLog("Header read\n", 0,0);

   			if (Canceled)	break;
			if(header.cupsWidth<=0) break;

   			Page ++;
   			DoLog("PAGE %d COPIES %d\n", Page, header.NumCopies);

   /*
    * Start the page...
    */
   			if ( !(CompData=tmpfile()) )
			{
       			perror("opening compressed page temp file"); 
       			abort(); 
   			} 

			OutBitsPerPixel = header.cupsBitsPerColor;

			if (header.cupsColorSpace == CUPS_CSPACE_CMY) //colour
			{
				DoLog("cupsColorSpace = %d = CUPS_CSPACE_CMY\n", header.cupsColorSpace, 0);
				fprintf(stderr, "INFO: c2espC: p%d Colour %d\n", Page, ColourPercent);
				sprintf(RasFileName,"/tmp/RasForComp.ppm");
			}
			else if (header.cupsColorSpace == CUPS_CSPACE_K)//monochrome
			{
				DoLog("cupsColorSpace = %d = CUPS_CSPACE_K\n", header.cupsColorSpace, 0);
				fprintf(stderr, "INFO: c2espC: p%d Monochrome\n",Page);
				sprintf(RasFileName,"/tmp/RasForComp.pbm");
			}
			else 
			{
				DoLog("Unknown cupsColorSpace = %d\n", header.cupsColorSpace, 0);
				DoLog("Allowed cupsColorSpace = %d or %d\n", CUPS_CSPACE_CMY, CUPS_CSPACE_K);
			}
			DoLog("cups raster w = %d h = %d\n", header.cupsWidth, header.cupsHeight);
			DoLog("cups raster bytes per line = %d bits per line = %d\n", header.cupsBytesPerLine,
				header.cupsBytesPerLine * 8);
#if DEBUGFILES == 1
			//open raster file here
			fprintf(stderr, "INFO: c2espC: Opening %s\n",RasFileName);
   			remove(RasFileName);
			dfp = fopen(RasFileName, "w");
			UncompressedFile = fopen("/tmp/KodakUncompressed", "w"); //open the file
			sleep(3); //does this help chmod to work?
			chmod(RasFileName, S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
			chmod("/tmp/KodakUncompressed", S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it

   			if (dfp && header.cupsColorSpace == CUPS_CSPACE_CMY) 
			{
				fprintf(dfp, "P6\n%8d %8d %8d\n", header.cupsWidth, header.cupsHeight, 255);
			}
   			if (dfp && header.cupsColorSpace == CUPS_CSPACE_K) 
			{
				fprintf(dfp, "P5\n%8d %8d %8d\n", header.cupsWidth, header.cupsHeight, 255);
			}
#endif

			AllocateBuffers(&header);
			MinOut=255;MaxOut=0; //initialise

 			if(Page == 1) SetupJob(&header);
			StartPrinterPage( &header);

			//prepare for compression
			DoLog("initialising Compression\n",0,0);

   			/* allocate deflate state */
   			strm.zalloc = Z_NULL;
   			strm.zfree = Z_NULL;
   			strm.opaque = Z_NULL;
   			ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
   			if (ret != Z_OK) 
			{ //was return ret;
				DoLog("Deflate did not initialise properly\n", 0, 0);
			}
        				
   			for (y = 0; (y < header.cupsHeight); ++y )  // Loop for each line of the page
   			{
   				if (Canceled) break;
				/* Keep the printer connection awake, added if (DoBack) 15/1/12 */
				if (DoBack) KeepAwakeStart = KeepAwake(KeepAwakeStart,10, PrintFile); 

				//read a line 
				if (!cupsRasterReadPixels(ras, CupsLineBuffer, header.cupsBytesPerLine)) break;
				//check for max and min either colour or mono
				for(i=0;i<(header.cupsBytesPerLine);++i)
				{
					if (CupsLineBuffer[i]>MaxOut) MaxOut=CupsLineBuffer[i];
					if (CupsLineBuffer[i]<MinOut) MinOut=CupsLineBuffer[i];
				}
				//turn the line into print data

				if (header.cupsColorSpace == CUPS_CSPACE_CMY) //colour - should be 3 x 8 bits per pixel
				{
					//count left blank pixels for colour - edit me
					for(PixelsLeft=0;(Composite(&CupsLineBuffer[0], PixelsLeft)==0 
						&& PixelsLeft<header.cupsWidth);++PixelsLeft);
					//count right blank pixels for colour - edit me
					if(PixelsLeft==header.cupsWidth) PixelsRight=0;
					else for(PixelsRight=0;Composite(&CupsLineBuffer[0], 
						header.cupsWidth-1-PixelsRight)==0 && (PixelsRight < header.cupsWidth);++PixelsRight);

				} //end of colour section

				else //monochrome - should be 8 bits per pixel
				{
					//count left blank pixels for mono
					for(PixelsLeft=0;(PixelsLeft<header.cupsWidth) && CupsLineBuffer[PixelsLeft]==0; ++PixelsLeft);
					//count right blank pixels for mono
					if(PixelsLeft==header.cupsWidth) PixelsRight=0;
					else for(PixelsRight=0;((PixelsRight<header.cupsWidth) && (CupsLineBuffer[header.cupsWidth-1-PixelsRight]==0)); 
						++PixelsRight);
				} //end of mono
// common mono and colour
				PixelsNonBlank=header.cupsWidth-PixelsLeft-PixelsRight;
				//log the counts
	//fprintf(LogFile,"%d+%d+%d=%d\n",PixelsLeft,PixelsNonBlank,PixelsRight,PixelsLeft+PixelsRight+PixelsNonBlank);
				//generate the data
				//LHS blanks - maybe should not do if PixelsLeft==0 ? need a test file
				DataLineBuffer[0]=Byte0(PixelsLeft);
				DataLineBuffer[1]=Byte1(PixelsLeft);
				DataLineBuffer[2]=0x01;
				DataLineBuffer[3]=0x00;
				DataLength=4;

				if(PixelsLeft<header.cupsWidth) //there are some non blank pixels
				{
					DataLineBuffer[4]=Byte0(PixelsNonBlank);
					DataLineBuffer[5]=Byte1(PixelsNonBlank);
					DataLineBuffer[6]=0x00;
					DataLineBuffer[7]=0x00;
					DataLength+=4;

					if (header.cupsColorSpace == CUPS_CSPACE_CMY) //colour - should be 3 x 8 bits per pixel
					{
						for(i=0;i<PixelsNonBlank;++i) 
						{
							DataLineBuffer[8+i]=CupsLineBuffer[(PixelsLeft+i)*3+0];
							DataLineBuffer[8+PixelsNonBlank+i]=CupsLineBuffer[(PixelsLeft+i)*3+1];
							DataLineBuffer[8+PixelsNonBlank*2+i]=CupsLineBuffer[(PixelsLeft+i)*3+2];
						}
						DataLength+=PixelsNonBlank*3;
					}
					else // its mono 1 x 8 bits per pixel
					{
						for(i=0;i<PixelsNonBlank;++i) DataLineBuffer[8+i]=CupsLineBuffer[PixelsLeft+i];
						DataLength+=PixelsNonBlank;
					}
				// for colour and mono						
					if(PixelsRight>0) //there are some RHS blanks
					{
							DataLineBuffer[DataLength+0]=Byte0(PixelsRight);
							DataLineBuffer[DataLength+1]=Byte1(PixelsRight);
							DataLineBuffer[DataLength+2]=0x01;
							DataLineBuffer[DataLength+3]=0x00;
							DataLength+=4;
					}
				}
				if(UncompressedFile) fwrite(DataLineBuffer, 1, DataLength, UncompressedFile);

				//send data to the compression
        		strm.next_in = DataLineBuffer;
				strm.avail_in = DataLength;
				if(y >= header.cupsHeight-1) flush=Z_FINISH;
				else flush = Z_NO_FLUSH;

        		/* run deflate() on input until output buffer not full  */
        		do 
				{
            		strm.avail_out = CHUNK;
            		strm.next_out = out;
//		DoLog("deflating %d byte input\n",strm.avail_in,0);
            		ret = deflate(&strm, flush);
					if(ret == Z_STREAM_ERROR) break;
            		have = CHUNK - strm.avail_out;
					if(have>0) DoLog("writing deflated output %d bytes\n",have,0);
            		if (fwrite(out, 1, have, CompData) != have || ferror(CompData)) 
					{
                		(void)deflateEnd(&strm);
						ret = Z_STREAM_ERROR; //kludge
						break;
            		}
        		} while (strm.avail_out == 0);

				if(ret == Z_STREAM_ERROR) break;

#if DEBUGFILES == 1
//store the raster for debugging - may need changing for colour
	    		if (dfp) fwrite(CupsLineBuffer, 1, header.cupsBytesPerLine, dfp);
#endif

    		} //end of line loop
			if(ret == Z_STREAM_ERROR)
			{
				DoLog("Compression error no %d\n",Z_ERRNO,0);
			}

 			/* end of compression */
			//put compressed byte count into output file and copy compressed data in
    		(void)deflateEnd(&strm);
			DoLog("Compression finished\n",0,0);
			DoLog("Rewinding compressed page file\n",0,0);
			rewind(CompData);

/* Old version sent data all in one go. This seems to cause a problem with smb shared printers.

			DoOutJob(PrintFile, "RasterObject.Data#%d=", strm.total_out,0);
			CheckCount=0;
			while((c=fgetc(CompData)) != EOF) 
			{
#if DEBUGFILES == 1
				fputc(c,PrintFile);
#endif
#if TESTING == 0
				fputc(c,stdout);
#endif
				++CheckCount;
			}
			DoOutJob(PrintFile, ";",0,0); //one semi colon after the data
			fflush(PrintFile);
*/

// New version with chunk size limit 15/1/12
			CheckCount=0;
			BytesToPrint = strm.total_out; 
			CurrentChunkSize = 0; 
			while (BytesToPrint > 0) { 
				if (BytesToPrint <= MAXDATACHUNK) { 
					CurrentChunkSize = BytesToPrint; 
				} 
				else { 
					CurrentChunkSize = MAXDATACHUNK; 
				} 
				//send one chunk of data 
				DoOutJob(PrintFile, "RasterObject.Data#%d=", CurrentChunkSize,0);
				//if(cbarg != NULL) rc = fwrite(CurrentChunkStart, 1, CurrentChunkSize, cbarg); 
				DoLog("GZIP data chunk is sent to printer\n",0,0); 
				//rc = fwrite(CurrentChunkStart, 1, CurrentChunkSize, stdout); //also to output 
				for(i=0;i<CurrentChunkSize;++i)
				{
					c=fgetc(CompData);
#if DEBUGFILES == 1
					fputc(c,PrintFile);
#endif
#if TESTING == 0
					fputc(c,stdout);
#endif
					++CheckCount;
				}

				DoOutJob(PrintFile, ";",0,0); //one semi colon after the chunk 
				BytesToPrint -= CurrentChunkSize; 
			} 

			DoLog("Compressed data copied to output expect %d read %d\n",strm.total_out,CheckCount);
 
			DoLog("Page raster built at %d sec\n",time(NULL)-StartTime,0);
			DoLog("Max and min in raster are %d %d\n",MaxOut,MinOut);

			//page is finished

#if DEBUGFILES == 1
			//close the debug files
			if(UncompressedFile) fclose(UncompressedFile);
			if(dfp) fclose(dfp);
#endif
    		EndPage();
   			if (Canceled)    break;
  		}
		while (cupsRasterReadHeader2(ras, &header));

	}
	else DoLog("no headers so nothing to print",0,0);

	if (Page > 0) ShutdownPrinter();

 /*
  * Close the raster stream... 
  */
 	cupsRasterClose(ras);
	DoLog("cups raster closed after %d sec\n",time(NULL)-StartTime,0);
  	if (fd != 0)   close(fd);
 /*
  * Termination, send an error message if required...
  */
	DoLog("c2espC terminating after %d sec. Processed %d pages\n",time(NULL)-StartTime,Page);

  	if (Page == 0)
  	{
    	DoLog("ERROR: c2espC: No pages found!\n",0,0);
    	return (1);
  	}
  	else
  	{
		CloseLogging();
		if(PrintFile != NULL) fclose(PrintFile);

#if DEBUGFILES == 1
		//let anyone read the files How much delay is needed if any?
		// sleep(3); //does this help chmod to work?
		chmod("/tmp/KodakCompPage", S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
		chmod("/tmp/KodakPrintLog", S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
		chmod("/tmp/KodakPrintFile", S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
#endif
		//cups seems to replace this by "Ready to print" so you don't see it
    	//fprintf(stderr, "INFO: Done - Blk %d Col %d percent\n",BlackPercent, ColourPercent);
		fprintf(stderr, "INFO: Done\n");
		return (0);
  	}
}
