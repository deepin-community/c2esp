/* 
 * Now using RGB for 300 and 600 dpi
 *   Kodak ESP 5xxx (OPL?) Control Language filter for the  Common UNIX
 *   Printing System (CUPS)
 *
 *  copyright Paul Newall May 2010 - Jan 2014. VERSION 2.7 (c2esp27) 
 *  patch by user awl29 applied to fix problems with non bi-directional printers, smb shared
 *  data chunk size limit applied
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
 *  Params: job-id user title copies options [file]
 *  options = "noback" disables all calls to the back channel for testing.
 *
This filter is based on:
the structure of the rastertohp filter supplied with cups
the JBIG library by Markus Kuhn

The ESP printers can print in black and white mode or in colour.
With resolutions 300 or 600 dpi.

If you want to compile with the DEBUGFILES or TESTING options, they should be set in c2espcommon.h
*/

#define MAXJBIGDATACHUNK 65511 /* some printers can't handle more than 65K at a time */

#include "../config.h"
#include <cups/raster.h>
#include <cups/sidechannel.h> //FlushBackChannel, and the side channel functions and constants
#include <fcntl.h> //files
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <jbig85.h> //the reduced jbig library
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
char	*Version = "c2esp27";
int StripeHeightMax = 1280; //the max height of a stripe. (Windows 300x1200 files have 1920)
const float	default_lut2[2] = {0.0, 1.0}; // for colour 1 bit
const float	default_lut3[3] = {0.0, 0.5, 1.0}; // for colour 2 bit or for black and grey will be separated to 2 x 1 bit
const float	default_lut5[5] = {0.0, 0.25, 0.5, 0.75, 1.0}; //for black and grey will be separated to 2 x 2 bit
int DoTrans = 1; //enables the cmyk transform
int MonitorColour = 0; //select colour for debug files CMYK 0123

char *RawColourFileName      = "/tmp/ColourRaw.pbm";
char *DitheredColourFileName = "/tmp/ColourDithered.pbm";
char *RasForCompFileName     = "/tmp/RasForComp.pbm";
char *CyanRasFileName        = "/tmp/RasCyan.pbm";
char *MagentaRasFileName     = "/tmp/RasMagenta.pbm";
char *YellowRasFileName      = "/tmp/RasYellow.pbm";
char *BlackRasFileName       = "/tmp/RasBlack.pbm";
char *GreyRasFileName        = "/tmp/RasGrey.pbm";
char *CupsRasterFileName     = "/tmp/CupsRaster.pbm";

char *PrintFileName          = "/tmp/KodakPrintFile";
char *LogFileName            = "/tmp/KodakPrintLog";
char *JobFileName            = "/tmp/c2espjob";

/*
 * Globals...
 */
unsigned char	*RasSpace;		/* Output buffer with space before start for previous lines */
unsigned char	*RasForComp;		/* Output buffer */
unsigned char	*CupsLineBuffer;	//buffer for one line of the cups raster, RGB or K, bytes
short		*TransformedBuffer;	//cups line buffer transformed, CMYK
short		*DitherInputBuffer;	//buffer for input to the dither, one colour, short ints
unsigned char	*DitherOutputBuffer;	//buffer for output of the dither, one colour, bytes
unsigned char	*CompStripeBuffer;	//buffer for one compressed stripe
char		KodakPaperSize[50];  	/* String that the printer expects for paper size */
int		CompStripeBufferFree,	//free pointer for buffer
		OutBitsPerPixel,	/* Number of bits per color per pixel for printer*/
		RasForCompWidth,		//width of raster that is compessed and sent to printer
		Duplex,			/* Current duplex mode */
		Page,			/* Current page number */
		Canceled,		/* Has the current job been canceled? */
		DoBack;			/* Enables the back channel comms */ 
int 		BytesPerColour;
int 		SkipStripe; //=1 for each blank stripe that will be skipped
int		kbUsed = 0; //tracks memory use
long		BytesOutCountSingle;//tracks total no of compressed bytes output by jbig library for single stripe system
time_t 		TimeStart; //to record the start of a section
FILE 		*PrintFile = NULL; //file descriptor for debug file
FILE		*JobFile;
	int			CupsLineBufMax = 0;
	int			TransformedBufMax = 0;
	unsigned char		MinOut, MaxOut; //to check the range of the dithered output
	short			MinIn=5000, MaxIn=0; //to check the range of the dithered input

time_t		StartTime;
time_t 		KeepAwakeStart;
int			StripeAccum; //to calc average pixel in stripe


#if DEBUGFILES == 1
FILE 		*dfp = NULL; //file descriptor for composite raster file
FILE 		*Cyanfp = NULL; //file descriptor for cyan only raster file
FILE 		*Magentafp = NULL; //file descriptor for magenta only raster file
FILE 		*Yellowfp = NULL; //file descriptor for yellow only raster file
FILE 		*Blackfp = NULL; //file descriptor for black only raster file
FILE 		*Greyfp = NULL; //file descriptor for grey only raster file
FILE 		*RawColourFile = NULL; //file descriptor for input to dither
FILE 		*DitheredColourFile = NULL; //file descriptor for output from dither
FILE 		*CupsRasterFile = NULL; //file descriptor for the cups raster
#endif

struct jbg85_enc_state JbigState;

/* DoOutJob used to enable one call to send to the specified job file and to stdout (if not testing)*/
void DoOutJobNoBack(FILE *OutFile, char *PrintFormat, int I1, int I2)
{
	if (OutFile) fprintf(OutFile, PrintFormat, I1, I2); //to the specified file
#if TESTING == 0
	fprintf(stdout, PrintFormat, I1, I2); //and to the output
#endif
	KeepAwakeStart = time(NULL); // reset timer
}

void
SetupPrinter(cups_page_header2_t *header)
{
//gets the printer ready to start the job
	int  i;
	int StatusLength;

	for(i=0; i<4; ++i)
	{
		if(GoodExchange(JobFile, "LockPrinterWait?", "0002, OK, Locked for printing;", DoBack,  1,  3.0) >= 0) break;
	}
	DoOutJob(JobFile, "Event=StartOfJob;",0,0); //printer command
        
        if (DoBack) {
		StatusLength=abs(GoodExchange(PrintFile, "DeviceStatus?", "0101,DeviceStatus.ImageDevice", DoBack,   1,  1.0));
		DoLog("StatusLength=%d\n",StatusLength,0);
/* you can get unexpected reply if there is an ink low warning then GoodExchange will be -ve */
//aquire ink levels here? DeviceStatus.Printer.InkLevelPercent.Colour=nn%&DeviceStatus.Printer.InkLevelPercent.Black=nn%
//note & used as separator
		if(StatusLength>0)
		{
			DoLog("ColourPercent=%d\n",ColourPercent,0);
			DoLog("BlackPercent=%d\n",BlackPercent,0);
    			fprintf(stderr,"ATTR: marker-levels=%d,%d\n",BlackPercent,ColourPercent); // sets the levels displayed in printer manager
		}
		GoodExchange(PrintFile, "DeviceSettings.System?", "0101,DeviceSettings.System", DoBack,   1,  1.0);
		GoodExchange(PrintFile, "DeviceSettings?", "0101,DeviceSettings.AddressBook", DoBack,   1,  1.0);
        }
        
	DoOutJob(JobFile, KodakPaperSize,0,0);

	if(header->MediaPosition == 0) DoOutJob(JobFile, "MediaInputTrayCheck=Main;",0,0);
	else if(header->MediaPosition == 1) DoOutJob(JobFile, "MediaInputTrayCheck=Photo;",0,0);
	else 
	{
		DoOutJob(JobFile, "MediaInputTrayCheck=Main;",0,0);
		DoLog("Unknown Input Tray no. %d so used main tray", header->MediaPosition, 0);
	}

        if (DoBack) {
		GoodExchange(PrintFile, "MediaTypeStatus?", "MediaTypeStatus=custom-media-type-deviceunavailable", DoBack,  1,  1.0);
		GoodExchange(PrintFile, "MediaDetect?", "0098, OK, Media Detect Started;", DoBack,   1,  1.0);
	//do MediaTypeStatus? until some media is found
#if TESTING == 0
		sleep(5); //typical media detect is 7 seconds
		for(i=0; i<15; ++i) //normal
#endif
#if TESTING == 1
		for(i=0; i<2; ++i) //short for tests
#endif
		{
			DoLog("MediaTypeStatus? try %d\n", i, 0);
			if(GoodExchange(PrintFile, "MediaTypeStatus?", "MediaTypeStatus=custom-media-type-deviceunavailable", DoBack,   2,  2.0) <= 0) break;
		}
	}
}

void
ShutdownPrinter(void)
{
	int i, ret;

	DoOutJob(JobFile, "Event=EndOfJob;",0,0);
	for(i=0; i<20; ++i) /* fast PC might need lots of tries here for printer to finish, how many is reasonable? */
	{
		/* First few tries will be quick so small jobs finish quickly */
		if(i<5) ret=GoodExchange(JobFile, "UnlockPrinter?", "0003, OK, Printer unlocked;", DoBack, 1,  5.0);
		/* Then tries will be slower so long jobs can finish */
		else ret=GoodExchange(JobFile, "UnlockPrinter?", "0003, OK, Printer unlocked;", DoBack, 5,  2.0);
		DoLog("UnlockPrinter? try %d returned %d\n", i, ret);
		if(ret >= 0 || strcmp(BackBuf , "3405, Error, Printer not locked") == 0) break;
		//error string has no terminating ; because it has been tokenised by GoodExchange
	}
}


void
SetupJob(cups_page_header2_t *header) //Prepare the printer for printing.
{
	DoLog("Called SetupJob(*header);\n",0,0);
	DoOutJob(JobFile, "OutputBin=MainSink;",0,0);

	Duplex = header->Duplex;
	if(Duplex == 0) DoOutJob(JobFile, "Sides=OneSided;",0,0);
	else  DoOutJob(JobFile, "Sides=TwoSided;",0,0);
	DoOutJob(JobFile, "MediaType=custom-media-type-autoselection-0-0-0-0;",0,0);
}

unsigned char *AllocateCharBuf(size_t Length, char *Name)
{
unsigned char *Buf;
  	if ((Buf = malloc(Length)) == NULL) 
  	{
		DoLog("ERROR: Unable to allocate %d bytes\n", Length, 0);
		DoLogString("ERROR: allocating array %s\n", Name);
    		exit(1);
  	}
	else 
	{
		DoLogString("INFO: allocated array %s\n", Name);
		DoLog("AllocateCharBuf:Address %d",(long) Buf,0);
		kbUsed += Length * 1E-3;
		return (Buf);
	}
}

short *AllocateShortBuf(size_t Length, char *Name)
{
short *Buf;
  	if ((Buf = malloc(Length * sizeof(short))) == NULL) 
  	{
		DoLog("ERROR: Unable to allocate %d bytes\n", Length * sizeof(short), 0);
		DoLogString("ERROR: allocating array %s\n", Name);
    		exit(1);
  	}
	else 
	{
		DoLogString("INFO: allocated array %s\n", Name);
		DoLog("Address %d",(long) Buf,0);
		kbUsed += Length * 1E-3;
		return (Buf);
	}
}

void
AllocateBuffers(cups_page_header2_t *header)
{
	int i,   RasForCompSize;

 // Allocate memory for a stripe of graphics... 
 	RasForCompSize = RasForCompWidth * StripeHeightMax;
	RasSpace = AllocateCharBuf(RasForCompSize+RasForCompWidth*2*sizeof(unsigned char), "RasSpace");
	RasForComp = RasSpace + (RasForCompWidth*2); //points to the 3rd line, so 2 lines below RasForComp[0] can be used
	// Clear the page, so the watermark (or grey ink) is blank
//probably not required now
	for(i=0;i<RasForCompSize-1;++i)  RasForComp[i] = 0;

	CupsLineBuffer = AllocateCharBuf(header->cupsBytesPerLine, "CupsLineBuffer");
	DitherInputBuffer = AllocateShortBuf(header->cupsWidth, "DitherInputBuffer");
	TransformedBuffer = AllocateShortBuf(4 * header->cupsWidth, "TransformedBuffer");
	DitherOutputBuffer = AllocateCharBuf(header->cupsWidth, "DitherOutputBuffer");
	CompStripeBuffer = AllocateCharBuf(RasForCompWidth * StripeHeightMax, "CompStripeBuffer");

	DoLog("Buffers allocated %d kb\n",kbUsed,0);
}

void
StartPrinterPage(cups_page_header2_t *header)
{
	int  ResX, ResY;

 	//fprintf(stderr, "DEBUG: c2esp: StartPage\n");
	DisplayHeader(header);
	ResX = header->HWResolution[0];
	ResY = header->HWResolution[1];

	DoOutJob(JobFile, KodakPaperSize,0,0);
	DoOutJob(JobFile, "Event=StartOfPage;",0,0);
	DoOutJob(JobFile, "Origin.Top=1.0mm;Origin.Left=1.0mm;",0,0);
	if(ResX==300)
	{
		DoOutJob(JobFile, "PrintQuality=0;",0,0);
    		if (OutBitsPerPixel==2)	DoOutJob(JobFile, "PrintSpeed=3;",0,0);
		else 	DoOutJob(JobFile, "PrintSpeed=1;",0,0);
	}
	else if(ResX==600)
	{
		DoOutJob(JobFile, "PrintQuality=4096;",0,0);
    		if (OutBitsPerPixel==2)	DoOutJob(JobFile, "PrintSpeed=3;",0,0); //check print speeds
		else 	DoOutJob(JobFile, "PrintSpeed=1;",0,0);
	}
	else if(ResX==1200)
	{
		DoOutJob(JobFile, "PrintQuality=8192;",0,0);
    		if (OutBitsPerPixel==2)	DoOutJob(JobFile, "PrintSpeed=3;",0,0); //check print speeds
		else 	DoOutJob(JobFile, "PrintSpeed=4;",0,0);
	}
	DoOutJob(JobFile, "Resolution=%dx%d;", ResX, ResY);
	DoOutJob(JobFile, "RasterObject.BitsPerPixel=%d;",OutBitsPerPixel,0);

    	if (header->cupsColorSpace == CUPS_CSPACE_RGB)
	{
		DoOutJob(JobFile, "RasterObject.Planes=00FFFF,1P0000&FF00FF,1P0000&FFFF00,2P0000&000000,2T0000&000000,1P0000;",0,0); //for colour
		DoLog("CUPS_CSPACE_RGB (%d)\n",header->cupsColorSpace,0);
	}
	else if	 (header->cupsColorSpace == CUPS_CSPACE_K)
	{
		DoOutJob(JobFile, "RasterObject.Planes=000000,2T0000&000000,1P0000;",0,0); //for mono
		DoLog("CUPS_CSPACE_K  (%d)\n",header->cupsColorSpace,0);
	}
	else	
	{
//add error message here
		DoOutJob(JobFile, "RasterObject.Planes=000000,2T0000&000000,1P0000;",0,0); //for mono
		DoLog("CUPS_CSPACE_??  (%d)\n",header->cupsColorSpace,0);
	}

	DoOutJob(JobFile, "RasterObject.Compression=JBIG;",0,0);
    	DoOutJob(JobFile, "RasterObject.Width=%d;", header->cupsWidth,0);
	DoOutJob(JobFile, "RasterObject.Height=%d;",  header->cupsHeight,0);
}

void
EndPage(void) //Finish a page of graphics.
{
	DoOutJob(JobFile, "Event=EndOfPage;",  0,0);

	/* Free memory... allocated by AllocateBuffers() */
//  	RasForComp not freed because it is inside RasSpace
  	free(RasSpace);
  	free(CupsLineBuffer);
  	free(DitherInputBuffer);
  	free(DitherOutputBuffer);
  	free(CompStripeBuffer);
	kbUsed = 0;
	DoLog("Buffers freed\n",0,0);
}

void
CancelJob(int sig)	/* - Cancel the current job... I - Signal */
{
  (void)sig;
  DoLog("CancelJob: job cancelled by signal\n",0,0);
  Canceled = 1;
}

/*
//look up table to map bit pair 11 to 10, 10 to 01, 01 to 01, 00 to 00
//translates 2 bit per pixel colours to printer data
unsigned char	Map11To10And10To01[256] =
{
0, 1, 1, 2, 4, 5, 5, 6, 4, 5, 5, 6, 8, 9, 9, 10, 
16, 17, 17, 18, 20, 21, 21, 22, 20, 21, 21, 22, 24, 25, 25, 26, 
16, 17, 17, 18, 20, 21, 21, 22, 20, 21, 21, 22, 24, 25, 25, 26, 
32, 33, 33, 34, 36, 37, 37, 38, 36, 37, 37, 38, 40, 41, 41, 42, 
64, 65, 65, 66, 68, 69, 69, 70, 68, 69, 69, 70, 72, 73, 73, 74, 
80, 81, 81, 82, 84, 85, 85, 86, 84, 85, 85, 86, 88, 89, 89, 90, 
80, 81, 81, 82, 84, 85, 85, 86, 84, 85, 85, 86, 88, 89, 89, 90, 
96, 97, 97, 98, 100, 101, 101, 102, 100, 101, 101, 102, 104, 105, 105, 106, 
64, 65, 65, 66, 68, 69, 69, 70, 68, 69, 69, 70, 72, 73, 73, 74, 
80, 81, 81, 82, 84, 85, 85, 86, 84, 85, 85, 86, 88, 89, 89, 90, 
80, 81, 81, 82, 84, 85, 85, 86, 84, 85, 85, 86, 88, 89, 89, 90, 
96, 97, 97, 98, 100, 101, 101, 102, 100, 101, 101, 102, 104, 105, 105, 106, 
128, 129, 129, 130, 132, 133, 133, 134, 132, 133, 133, 134, 136, 137, 137, 138, 
144, 145, 145, 146, 148, 149, 149, 150, 148, 149, 149, 150, 152, 153, 153, 154, 
144, 145, 145, 146, 148, 149, 149, 150, 148, 149, 149, 150, 152, 153, 153, 154, 
160, 161, 161, 162, 164, 165, 165, 166, 164, 165, 165, 166, 168, 169, 169, 170, 
};

void MapDotsInByte(unsigned char *Byte)
{
	*Byte = Map11To10And10To01[*Byte];
}
*/
void
output_jbig(unsigned char *start, size_t len, void *cbarg)
/* 
start is a pointer to some JBIG data
len is the number of bytes of JBIG data
cbarg is a pointer to an optional output file
(output is also sent to stdout)

uses a fixed global buffer to store one stripe and sends stripe when it becomes complete
 Includes mod by Andreas (awl29) to limit the size of JBIG data chunks which is required for some older models
*/

{
int	i;
int	rc;
int BytesToPrint; 
int CurrentChunkSize; 
unsigned char *CurrentChunkStart; 
 //copy bytes one at a time looking for end of BIE and counting
	for(i=0;i<len;++i)
	{
		CompStripeBuffer[CompStripeBufferFree] = start[i];
		++CompStripeBufferFree;
		++BytesOutCountSingle;

//at end of BIE call output functions
		if(CompStripeBufferFree >= 2 && BytesOutCountSingle > 20) //only if the header is complete
		{
			if(CompStripeBuffer[CompStripeBufferFree-2] == ESC && CompStripeBuffer[CompStripeBufferFree-1] != 0)
			{
			//end of BIE detected
				DoLog("SingleStripe: detected end of BIE at %d bytes, length %d\n",BytesOutCountSingle,CompStripeBufferFree);
			//send the buffer to the printer, in chunks less than MAXJBIGDATACHUNK
				BytesToPrint = CompStripeBufferFree; 
				CurrentChunkSize = 0; 
				CurrentChunkStart = CompStripeBuffer; 
				while (BytesToPrint > 0) { 
					if (BytesToPrint <= MAXJBIGDATACHUNK) CurrentChunkSize = BytesToPrint; 
					else CurrentChunkSize = MAXJBIGDATACHUNK; 
			//send one chunk of data 
					DoOutJob(cbarg, "RasterObject.Data#%d=", CurrentChunkSize,0); 
					if(cbarg != NULL) rc = fwrite(CurrentChunkStart, 1, CurrentChunkSize, cbarg); 
					DoLog("JBIG data chunk is sent to printer\n",0,0); 
					rc = fwrite(CurrentChunkStart, 1, CurrentChunkSize, stdout); //also to output 
					DoOutJob(cbarg, ";",0,0); //one semi colon after the chunk 
					BytesToPrint -= CurrentChunkSize; 
					CurrentChunkStart += CurrentChunkSize; 
				} 

			//then restart the buffer
				CompStripeBufferFree = 0;
			}
		}
	}
}


void OutputLineExtents(unsigned char *buf, int Totalbpl, cups_page_header2_t header, FILE *fp)
{
	int BandLeft, BandRight, BytesLeft, BytesRight, ThisBytePrint, BytesPerCol, Col, Cols, Printbpl;
//BitsPerPixel is in the buffer of printer data, not in the cups raster Use global OutBitsPerPixel
	Printbpl = (header.cupsWidth + 7) / 8;
	if (header.cupsColorSpace == CUPS_CSPACE_CMYK) Cols=4;
	else if (header.cupsColorSpace == CUPS_CSPACE_RGB) Cols=4;
	else Cols=1;
	BytesPerCol=Totalbpl/(Cols+1);
	//Search for extents in this row of pixels
	for(BytesLeft=0;BytesLeft<Printbpl;++BytesLeft)
	{
		ThisBytePrint=0;
		for(Col=0;Col<Cols;++Col)
		{
			if(buf[BytesLeft+BytesPerCol*Col]!=0) ThisBytePrint=1;
		}
		if(ThisBytePrint==1) break;
	}
	if(BytesLeft >= Printbpl) BandLeft=0;
	else BandLeft=BytesLeft*8/OutBitsPerPixel;
	//BytesLeft is the offset to the first byte with pixels
	//BandLeft is the pixel offset to the first byte that is not blank

	for(BytesRight=Printbpl*OutBitsPerPixel-1;BytesRight >= 0;--BytesRight)
	{
		ThisBytePrint=0;
		for(Col=0;Col<Cols;++Col)
		{
			if(buf[BytesRight+BytesPerCol*Col]!=0) ThisBytePrint=1;
		}
		if(ThisBytePrint==1) break;
	}
	if(BytesRight < 0) BandRight=0;
	else BandRight=(BytesRight+1)*8/OutBitsPerPixel;
	//BytesRight is the off set to the last byte with pixels
	//BandRight is the pixel offset to the byte after last byte that is not blank
	DoOutJobNoBack(fp,",%d,%d",BandLeft,BandRight);
}

void PrintOneStripe (unsigned char *buf, int Stripe, int StripeHeight, cups_page_header2_t header, FILE *fp)
{
// buf is the stripe buffer with width w and height StripeHeight pixels. PrintW PrintH are the pixel width and height of the print raster
    int	 y, Index;
	DoLog("PrintOneStripe starts SkipStripe = %d\n",SkipStripe,0);
	DoLog("Extent search BitsPerPixel=%d\n",OutBitsPerPixel,0);
	//writes the extents and data for one band for the 5250 printer 
	DoLog("stripe %d of height %d\n",Stripe, StripeHeight);
	fprintf(stderr,"INFO: c2esp: Page %d Stripe %d\n", Page, Stripe);

	//black in bands was stored in SkipStripe
	if(SkipStripe)
	{
		//there is no print in this band
		DoOutJob(fp, "RasterObject.Extent=skip,%d;", StripeHeight,0);
	   	DoLog("<Stripe %d skipped>\n",Stripe, 0);
	}
	else
	{
		//there is print in this band
		DoOutJob(fp,"RasterObject.BandHeight=%d;",StripeHeight,0);
		DoOutJob(fp,"RasterObject.Extent=true",0,0);
		DoLog("sending the extent data\n",0,0);
		for(y=0;y<StripeHeight;++y) OutputLineExtents(&buf[(y)*RasForCompWidth], RasForCompWidth, header, fp);
		DoOutJob(fp,";",0,0); //extent terminator at end of band

		fprintf(stderr,"DEBUG: c2esp: Compress a stripe\n");
		for(y=0;y < StripeHeight;++y)
		{
			Index = (y)*RasForCompWidth;
			jbg85_enc_lineout(&JbigState, &buf[Index], &buf[Index - RasForCompWidth], &buf[Index - 2 * RasForCompWidth]);
		}

		DoOutJob(fp,"Event=EndOfBand;",0,0);
	} //end of stripe with print
}

void SetUpDither(cups_lut_t *Lut[], cups_dither_t *DitherState[], int LineWidth, int OutBits)
{
//Creates Luts and DitherStates for dithering

	int Col;
	for(Col=0;Col<3;++Col)
	{
		//For CMY or RGB
  		if(OutBits == 2) Lut[Col] = cupsLutNew(3, default_lut3);
		else Lut[Col] = cupsLutNew(2, default_lut2);
  		DitherState[Col] = cupsDitherNew(LineWidth);
	}
		//For Kk
  		if(OutBits == 2) Lut[3] = cupsLutNew(5, default_lut5);
		else Lut[3] = cupsLutNew(3, default_lut3);
  		DitherState[3] = cupsDitherNew(LineWidth);
}

unsigned char Dithered8ToPrint(unsigned char *Buffer, int x, int Level)
{
	//gets 8 points from Buffer started with x and maps them into one byte for one ink or light ink
	//Buffer must be >= 8 chars longer than x
	unsigned char Build=0;
/*	if (Buffer[x+0]==Level) Build+=0b10000000;
	if (Buffer[x+1]==Level) Build+=0b01000000;
	if (Buffer[x+2]==Level) Build+=0b00100000;
	if (Buffer[x+3]==Level) Build+=0b00010000;
	if (Buffer[x+4]==Level) Build+=0b00001000;
	if (Buffer[x+5]==Level) Build+=0b00000100;
	if (Buffer[x+6]==Level) Build+=0b00000010;
	if (Buffer[x+7]==Level) Build+=0b00000001;
*/
	if (Buffer[x+0]==Level) Build+=128; //set bit 7
	if (Buffer[x+1]==Level) Build+=64; //set bit 6
	if (Buffer[x+2]==Level) Build+=32;
	if (Buffer[x+3]==Level) Build+=16;
	if (Buffer[x+4]==Level) Build+=8;
	if (Buffer[x+5]==Level) Build+=4;
	if (Buffer[x+6]==Level) Build+=2;
	if (Buffer[x+7]==Level) Build+=1; //set bit 0
	return Build;
}


unsigned char Dithered4ToPrint(unsigned char *Buffer, int x)
{
	//gets 4 points from Buffer started with x and maps them into one byte for one ink or light ink
	//Buffer must be >= 4 chars longer than x
	// (0->00,  1->01, 2->10,3->00, 4->00)
	unsigned char Build=0;
/*	if (Buffer[x+0]==1) Build+=0b01000000;
	if (Buffer[x+0]==2) Build+=0b10000000;
	if (Buffer[x+1]==1) Build+=0b00010000;
	if (Buffer[x+1]==2) Build+=0b00100000;
	if (Buffer[x+2]==1) Build+=0b00000100;
	if (Buffer[x+2]==2) Build+=0b00001000;
	if (Buffer[x+3]==1) Build+=0b00000001;
	if (Buffer[x+3]==2) Build+=0b00000010;
*/
	if (Buffer[x+0]==2) Build+=128; //set bit 7
	if (Buffer[x+0]==1) Build+=64;
	if (Buffer[x+1]==2) Build+=32;
	if (Buffer[x+1]==1) Build+=16;
	if (Buffer[x+2]==2) Build+=8;
	if (Buffer[x+2]==1) Build+=4;
	if (Buffer[x+3]==2) Build+=2;
	if (Buffer[x+3]==1) Build+=1; //set bit 0
	return Build;
}

unsigned char Dithered4ToPrintDark(unsigned char *Buffer, int x)
{
	//gets 4 points from Buffer started with x and maps them into one byte for dark ink
	//Buffer must be >= 4 chars longer than x
	// (0->00, 1->00, 2->00, 3->01, 4->10)
	unsigned char Build=0;
/*	if (Buffer[x+0]==3) Build+=0b01000000;
	if (Buffer[x+0]==4) Build+=0b10000000;
	if (Buffer[x+1]==3) Build+=0b00010000;
	if (Buffer[x+1]==4) Build+=0b00100000;
	if (Buffer[x+2]==3) Build+=0b00000100;
	if (Buffer[x+2]==4) Build+=0b00001000;
	if (Buffer[x+3]==3) Build+=0b00000001;
	if (Buffer[x+3]==4) Build+=0b00000010;
*/
	if (Buffer[x+0]==4) Build+=128; //set bit 7
	if (Buffer[x+0]==3) Build+=64;
	if (Buffer[x+1]==4) Build+=32;
	if (Buffer[x+1]==3) Build+=16;
	if (Buffer[x+2]==4) Build+=8;
	if (Buffer[x+2]==3) Build+=4;
	if (Buffer[x+3]==4) Build+=2;
	if (Buffer[x+3]==3) Build+=1; //set bit 0
	return Build;
}

void DummyTransform(unsigned char *InBuffer, short *OutBuffer, int Pixels)
{
	//substitute for the cups transform for testing
	int x;
	for(x=0;x<Pixels;++x) OutBuffer[x]=((short)InBuffer[x])<<4;
}

void InsertGradientChunky(unsigned char *Buffer, cups_page_header2_t *header, int y)
{
	//fills the buffer with a gradient 0-255 for testing
	int x;
	if (header->cupsColorSpace == CUPS_CSPACE_RGB)
	{
		if(y==0) DoLog("Inserting RGB gradient at top of page\n",0,0);
		for(x=0;x<header->cupsWidth*3;x+=3) 
		{
			Buffer[x]=255*x/(header->cupsWidth*3);
			Buffer[x+1]=Buffer[x];
			Buffer[x+2]=Buffer[x];
		}
	}
	else if (header->cupsColorSpace == CUPS_CSPACE_K)
	{
		if(y==0) DoLog("Inserting K gradient at top of page\n",0,0);
		for(x=0;x<header->cupsWidth;++x) Buffer[x]=255*x/(header->cupsWidth);
	}
}

FILE *OpenPbm(char *Name, int Width, int Height, int FullScale)
{
// Opens a pbm graphic file for writing. "P5" or "P4"
// If FullScale = 0, a P4 file (line art, 1 bit per pixel)
// If FullScale > 0, a P5 file (gray scale, 1 byte per pixel)

	FILE *Handle;
	remove(Name);
	Handle = fopen(Name, "w");
	if (Handle) 
	{
		if(FullScale == 0) fprintf(Handle, "P4\n%8d %8d\n", Width, Height);
		else if (FullScale > 0) fprintf(Handle, "P5\n%8d %8d %8d\n", Width, Height, FullScale);
		DoLogString("Opened %s\n", Name);
	}
	else DoLogString("Could not open %s\n", Name);
	return Handle;
}

void LetAllRead(char *Fname)
{
		chmod(Fname, S_IRUSR | S_IWUSR | S_IROTH ); //let anyone read it
}


void 
Terminate(cups_raster_t *ras,int fd,cups_dither_t **DitherState,cups_lut_t **Lut)
{
int CloseError, Col;

 /*
  * Close the raster stream... and the debug file
  */
 	cupsRasterClose(ras);
	DoLog("cups raster closed after %d sec\n",time(NULL)-StartTime,0);
  	if (fd != 0)   close(fd);
	if(JobFile != NULL)
	{
		CloseError = fclose(JobFile);
		DoLog("jobfile closed after %d sec with return value %d\n",time(NULL)-StartTime, CloseError);
	}

//free the dither states
  	for(Col = 0; Col < 4 ;++Col)
	{
		cupsDitherDelete(DitherState[Col]);
	  	cupsLutDelete(Lut[Col]);
	}
 /*
  * Termination, send an error message if required...
  */
	DoLog("Bytes output by JBIG just before terminating = %d\n",BytesOutCountSingle,0);
	DoLog("c2esp terminating after %d sec. Processed %d pages\n",time(NULL)-StartTime,Page);
  	if (Page == 0)
  	{
    		fprintf(stderr, ("ERROR: c2esp: No pages found!\n"));
    		//return (1);
  	}
  	else
  	{
		CloseLogging();

		//cups seems to replace this by "Ready to print" so you don't see it
                if (DoBack) fprintf(stderr, "INFO: c2esp: Ready to print Blk %d Col %d percent\n",BlackPercent, ColourPercent);
		else fprintf(stderr, "INFO: c2esp: Ready to print (no bi-di communication)\n");

#if DEBUGFILES == 1
		if(PrintFile != NULL) fclose(PrintFile);

		LetAllRead(PrintFileName);
		LetAllRead(LogFileName);
		LetAllRead(JobFileName);
#endif
  	}
}

void
SaveDitherOut(cups_page_header2_t *header, int Col, int BlankColour,FILE *DitheredColourFile,FILE *CupsRasterFile)
{
	//save one line in files, 
	//DitherOutputBuffer[x] into DitheredColourFile
	//CupsLineBuffer[x] into CupsRasterFile
	//in the files 255=white 0=black (reverse of printer)

	int output=0;
	int x;

	if(Col == MonitorColour) //0=cyan 1=mag 2=yellow 3=black
	{
//DoLog("SaveDitherOut BlankColour %d Resolution %d\n",BlankColour,header->HWResolution[0]);
		for(x=0;x<header->cupsWidth;++x) 
		{
      			if(BlankColour==0 && header->HWResolution[0] == 300) 
			{
				//dithered (7 bit shift to approximate 255/2)
				if(Col<3)output = 255 - (DitherOutputBuffer[x] * 255);
				//Black and grey
				if(Col==3) output = 255 - (DitherOutputBuffer[x] * 255/2);
      				if (output < 0) output = 0;
			}
      			else if(BlankColour==0 && header->HWResolution[0] == 600) 
			{
				if(Col<3)output = 255 - (DitherOutputBuffer[x] * 255/2);
				//Black and grey
				if(Col==3) output = 255 - (DitherOutputBuffer[x] * 255/4);
      				if (output < 0) output = 0;
			}
			else 
			{
				output=255;
				//MinOut=0;
			}
      			if (DitheredColourFile) fputc(output, DitheredColourFile);

			if(BlankColour==0 && header->cupsColorSpace == CUPS_CSPACE_RGB) 
			{
				//cups raster chunky version
				if (Col < 3 ) output = (CupsLineBuffer[x*3+Col]);
				//next line should really be the min of the 3 colours?
				if (Col == 3 ) 
				{
					output = (CupsLineBuffer[x*3+0]+CupsLineBuffer[x*3+1]+CupsLineBuffer[x*3+2])/3;
				}
      				if (output < 0) output = 0;
			}
			else if(BlankColour==0 && header->cupsColorSpace == CUPS_CSPACE_K) 
			{
				output = (255-CupsLineBuffer[x]);
			}
			else output=255;

			if (CupsRasterFile) fputc(output, CupsRasterFile);
		}
	}
}

void DitherProcess(cups_lut_t **Lut, cups_dither_t **DitherState, cups_page_header2_t *header, int Col, int *BlankColour)
{
	//convert the bits in CupsLineBuffer to short ints in DitherInputBuffer for the current colour
	//checking if it's blank as we go
#if DEBUGFILES==1
	int output;
#endif
	int x;
	*BlankColour=1; //if it remains 1 the line is blank and we don't need to dither.
	if(header->cupsColorSpace == CUPS_CSPACE_RGB)
	{
		for(x=0;x<header->cupsWidth;++x) 
		{
			//CupsLineBuffer RGB chunky section - TransformedBuffer is CMYK chunky
			if(CupsLineBuffer[3*x+Col] > CupsLineBufMax) CupsLineBufMax = CupsLineBuffer[3*x+Col];
			if(TransformedBuffer[4*x+Col] > TransformedBufMax) TransformedBufMax = TransformedBuffer[4*x+Col];
			if(DoTrans == 1) DitherInputBuffer[x]=TransformedBuffer[4*x+Col];
			else DitherInputBuffer[x]=CupsLineBuffer[3*x+Col]<<4; //scale to 4096
			if(DitherInputBuffer[x]>0) *BlankColour=0;
		}
	}
	else if(header->cupsColorSpace == CUPS_CSPACE_K)
	{
		for(x=0;x<header->cupsWidth;++x) 
		{
			//CupsLineBuffer K section
			if(CupsLineBuffer[x] > CupsLineBufMax) CupsLineBufMax = CupsLineBuffer[x];
			DitherInputBuffer[x]=CupsLineBuffer[x]<<4; //scale to 4096
			if(DitherInputBuffer[x]>0) *BlankColour=0;
		}
	}
	else DoLog("Unsupported cspace %d\n",header->cupsColorSpace,0);

#if DEBUGFILES==1
	//save in file, in the file 255=white 0=black(reverse of printer)
	if(Col == MonitorColour) //0=cyan 1=mag 2=yellow 3=black
	{
		for(x=0;x<header->cupsWidth;++x) 
		{
			output = 255 - (DitherInputBuffer[x]>>4);
      			if (output < 0) output = 0;
      			if (RawColourFile) fputc(output, RawColourFile);
			if (DitherInputBuffer[x]>MaxIn) MaxIn=DitherInputBuffer[x];
			if (DitherInputBuffer[x]<MinIn) MinIn=DitherInputBuffer[x];
		}
	}
#endif
	//dither the line. Luts have been loaded to suit the bits per colour.
	//if(y == 0) DoLog("Dither stage colour %d line %d\n", Col, y);
	if(*BlankColour==0) 
	{
		cupsDitherLine(DitherState[Col], Lut[Col], DitherInputBuffer, 1, DitherOutputBuffer);
		// full scale input is 4095. output is the index in the lut.
	}
	if(Col == MonitorColour) //0=cyan 1=mag 2=yellow 3=black
	{
		for(x=0;x<header->cupsWidth;++x) //added loop structure 17/12/13
		{
			if (DitherOutputBuffer[x]>MaxOut) MaxOut=DitherOutputBuffer[x];
			if (DitherOutputBuffer[x]<MinOut) MinOut=DitherOutputBuffer[x];
		}
		if(*BlankColour!=0) MinOut=0;
	}
//DoLog("DitherProcess BlankColour %d Col %d\n",*BlankColour,Col);

#if DEBUGFILES==1
	SaveDitherOut(header, Col, *BlankColour, DitheredColourFile, CupsRasterFile);
#endif
}

void FakePutBitsIntoRaster(cups_page_header2_t *header, int Col, int BlankColour, int y)
{
	//put the bits into the appropriate position in the printer raster
	int BlackIndex, x; //BlackIndex is the band in the printer raster

	if(header->cupsColorSpace == CUPS_CSPACE_RGB) BlackIndex=3;
	else BlackIndex=0;
	if(BlankColour == 0 && OutBitsPerPixel == 2)
	{
		if(y==0) DoLog("Copying to raster col %d bpp %d\n", Col, OutBitsPerPixel);
//		for(x=0;x<header->cupsWidth;x=x+4) 
//		{
x=50;
			if( Col == 3)
			{
				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/4]=255;
				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/4]=255;
//				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/4]=Dithered4ToPrintDark(DitherOutputBuffer,x);
//				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/4]=Dithered4ToPrint(DitherOutputBuffer,x);
			}
			else RasForComp[y * RasForCompWidth + Col * (BytesPerColour)+x/4]=Dithered4ToPrint(DitherOutputBuffer,x);
			//hope that ras is zero initially
//		}
	}
	if(BlankColour == 0 && OutBitsPerPixel == 1)
	{
		if(y==0) DoLog("Copying to raster col %d bpp %d\n", Col, OutBitsPerPixel);
		//1 bit out per colour
//		for(x=0;x<header->cupsWidth;x=x+8) 
//		{
x=50;
			if( Col == 3)
			{
				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/8]=255;
				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/8]=255;
//				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,2);
//				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,1);
			}
			else RasForComp[y * RasForCompWidth + Col * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,1);
//		}
	}

}

void PutBitsIntoRaster(cups_page_header2_t *header, int Col, int BlankColour, int y)
{
	//put the bits into the appropriate position in the printer raster
	int BlackIndex, x; //BlackIndex is the band in the printer raster

	if(header->cupsColorSpace == CUPS_CSPACE_RGB) BlackIndex=3;
	else BlackIndex=0;
	if(BlankColour == 0 && OutBitsPerPixel == 2)
	{
		if(y==0) DoLog("Copying to raster col %d bpp %d\n", Col, OutBitsPerPixel);
		for(x=0;x<header->cupsWidth;x=x+4) 
		{
			if( Col == 3)
			{
				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/4]=Dithered4ToPrintDark(DitherOutputBuffer,x);
				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/4]=Dithered4ToPrint(DitherOutputBuffer,x);
			}
			else RasForComp[y * RasForCompWidth + Col * (BytesPerColour)+x/4]=Dithered4ToPrint(DitherOutputBuffer,x);
			//hope that ras is zero initially
		}
	}
	if(BlankColour == 0 && OutBitsPerPixel == 1)
	{
		if(y==0) DoLog("Copying to raster col %d bpp %d\n", Col, OutBitsPerPixel);
		//1 bit out per colour
		for(x=0;x<header->cupsWidth;x=x+8) 
		{
			if( Col == 3)
			{
				RasForComp[y * RasForCompWidth + BlackIndex * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,2);
				RasForComp[y * RasForCompWidth + (BlackIndex+1) * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,1);
			}
			else RasForComp[y * RasForCompWidth + Col * (BytesPerColour)+x/8]=Dithered8ToPrint(DitherOutputBuffer,x,1);
		}
	}
}

/*
 * 'main()' - Main entry and processing of driver.
 */

int					/* O - Exit status */
main(int  argc,	char *argv[])		/* I - Number of command-line arguments, Command-line arguments */
{
  	int			fd;		/* File descriptor */
  	ppd_file_t		*ppd;		/* PPD file */
 	cups_raster_t		*ras;		/* Raster stream from cups */
  	cups_page_header2_t	header;		/* Page header from cups */
	cups_cmyk_t		*cmykTrans300;	/* cmyk transform for cups 300dpi*/
	cups_cmyk_t		*cmykTrans600;	/* cmyk transform for cups 600dpi*/
	int			RemainingPixels;
  	int			Stripe, y;		
	int			StripeEnd; //index of last byte in current stripe
	int			StripeHeight; //height of current stripe
	int			Col,i,x; 
	int			BlankColour; //boolean to record if the line is blank to save time
        int 			argi;
  	cups_lut_t		*Lut[4];		/* Dither lookup tables */
  	cups_dither_t		*DitherState[4];	/* Dither states */
	long			RasForCompHeight;

	StartTime = time(NULL);
	KeepAwakeStart = time(NULL);

	#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  	struct sigaction action;		/* Actions for POSIX signals */
	#endif 
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
        } else {
                bidi = CUPS_SC_BIDI_NOT_SUPPORTED;
                DoBack = 0;
        }

#if TESTING == 1
	DoBack = 0; //If testing never ask for replies
#endif

#if DEBUGFILES == 1
	SetupLogging("c2esp",DoBack,LogFileName);
#else
	SetupLogging("c2esp",DoBack,"");
#endif

  	setbuf(stderr, NULL);
      	fprintf(stderr, ("DEBUG:  ================ %s ====================================\n"),Version); 
	DoLogString("Starting %s\n",Version);
        DoLog("Compiled with DEBUGFILES = %d, TESTING = %d\n", DEBUGFILES, TESTING);
        
        DoLog("Number of command line parameters: %d\n", argc, 0);
        for (argi = 0; argi < argc; argi++) DoLogString("  param: '%s'\n", argv[argi]);
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
      			fprintf(stderr, ("ERROR: c2esp: Unable to open raster file - %s\n"),
                      	strerror(errno));
      			sleep(1);
      			return (1);
    		}
  	}
  	else    fd = 0;
      	DoLog("opening raster\n",0,0); 
  	ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
	if(! ras) DoLog("Problem opening cups raster",0,0);

 /*
  * Set up the colour separation system
  * 
  */
      	DoLogString("opening ppd %s\n",getenv("PPD")); 
  	ppd = ppdOpenFile(getenv("PPD"));
	if(ppd)
	{
      		DoLog("Opened ppd OK. Reading from ppd\n",0,0); 
		cmykTrans300 = cupsCMYKLoad(ppd, "RGB", "", "300x1200dpi");
		cmykTrans600 = cupsCMYKLoad(ppd, "RGB", "", "600x1200dpi");
		ppdClose(ppd);
     		DoLog("InkChannels 300dpi=%d 600dpi=%d\n",cmykTrans300->num_channels,cmykTrans600->num_channels);
	}
	else DoLogString("Problem opening ppd %s\n",getenv("PPD"));


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
	JobFile = fopen(JobFileName, "w"); 
	if (JobFile != NULL) DoLogString("JobFile %s Opened\n", JobFileName);
	else DoLogString("JobFile %s failed to open\n", JobFileName);

	PrintFile = fopen(PrintFileName, "w");
	if (PrintFile != NULL) DoLogString("PrintFile %s Opened\n", PrintFileName);
	else DoLogString("PrintFile %s failed to open\n", PrintFileName);
#endif

/* read the first header */
	if(cupsRasterReadHeader2(ras, &header) == 1)
	{
		DoLog("First page Header read after %d sec\n", time(NULL)-StartTime,0);

/* Check header for validity */
		if(HeaderInvalid(&header) == 1) 
		{
			Canceled = 1;
 		}
		else
		{
		if(header.HWResolution[0] == 300) OutBitsPerPixel = 1; 
		else OutBitsPerPixel = 2;

		SetUpDither(Lut, DitherState, header.cupsWidth, OutBitsPerPixel);

		SetupPrinter(&header);
		DoLog("Printer should be ready now\n",0,0);
		}
  /* 
  * Process pages as needed...
  */
  		Page = 0;
//start of loop for each page
		do 
  		{
			DoLog("Header read\n", 0,0);
	    		if (Canceled)	break;
			if(header.cupsWidth == -1) break;
    			Page ++;
	   		DoLog("PAGE %d width %d\n", Page, header.cupsWidth);

   /*
    * Start the page...
    */
			BytesOutCountSingle = 0; //initialise counter
			RasForCompHeight = 0; /* will accumulate the height for one page */
		//StartTime=time(NULL);
			SetPaperSize(KodakPaperSize, header.PageSize[1]); 
			if(header.HWResolution[0] == 300) OutBitsPerPixel = 1; 
			else OutBitsPerPixel = 2;
			BytesPerColour = (((header.cupsWidth * OutBitsPerPixel + 7) / 8) + 31) / 32 * 32; //round each colour up to 32 bytes

			if (header.cupsColorSpace == CUPS_CSPACE_RGB) //colour only RGB now
			{
				DoLog("Colour cups raster w = %d h = %d\n", header.cupsWidth, header.cupsHeight);
				DoLog("Colour cups bits per pixel = %d, bits per colour = %d\n", header.cupsBitsPerPixel, header.cupsBitsPerColor);
				DoLog("Colour cups raster bytes per line = %d bits per line = %d\n", header.cupsBytesPerLine, header.cupsBytesPerLine * 8);
				RasForCompWidth = BytesPerColour * 5;
				fprintf(stderr, "INFO: p%d Colour dithering\n",Page);
			}
			else if (header.cupsColorSpace == CUPS_CSPACE_K)//monochrome
			{
				fprintf(stderr, "INFO: p%d Monochrome\n",Page);
				DoLog("Mono cups raster w = %d h = %d\n", header.cupsWidth, header.cupsHeight);
				DoLog("Mono cups raster bytes per line = %d bits per line = %d\n", header.cupsBytesPerLine, header.cupsBytesPerLine * 8);
				RasForCompWidth = BytesPerColour * 2;
			}
			else 
			{
				DoLog("Unknown cupsColorSpace = %d\n", header.cupsColorSpace, 0);
				DoLog("Supported values are RGB %d and K %d\n", CUPS_CSPACE_RGB, CUPS_CSPACE_K);
			}

#if DEBUGFILES == 1
		//open dither files here
			RawColourFile = OpenPbm(RawColourFileName, header.cupsWidth, header.cupsHeight, 255);
			DitheredColourFile = OpenPbm(DitheredColourFileName,  header.cupsWidth, header.cupsHeight, 255);
		//open debug files
			dfp = OpenPbm(RasForCompFileName, RasForCompWidth * 8, StripeHeightMax, 0);
			Blackfp = OpenPbm(BlackRasFileName, BytesPerColour * 8, StripeHeightMax, 0);
			Greyfp = OpenPbm(GreyRasFileName, BytesPerColour * 8, StripeHeightMax, 0);
			CupsRasterFile = OpenPbm(CupsRasterFileName, header.cupsWidth, header.cupsHeight, 255);
			if(header.cupsColorSpace==CUPS_CSPACE_K)
			{
				DoLog("Deleting any existing CMY raster files",0,0);
				remove(CyanRasFileName);
				remove(MagentaRasFileName);
				remove(YellowRasFileName);
			}
			else
			{
				Cyanfp = OpenPbm(CyanRasFileName, BytesPerColour * 8, StripeHeightMax, 0);
				Magentafp = OpenPbm(MagentaRasFileName, BytesPerColour * 8, StripeHeightMax, 0);
				Yellowfp = OpenPbm(YellowRasFileName, BytesPerColour * 8, StripeHeightMax, 0);
			}
#endif
			if(header.cupsWidth == -1)
			{
				DoLog("Width %d is <= 0? so not allocating buffers\n",header.cupsWidth,0);
				Terminate(ras,fd,DitherState,Lut);
				exit(0);
			}
			else 
			{
				DoLog("Width %d is >0? so allocating buffers\n",header.cupsWidth,0);
				AllocateBuffers(&header);

			// test buffers
			//if(!CupsLineBuffer) DoLog("missing CupsLineBuffer\n",0,0);
			//else DoLog("Main:Address %d",(long) CupsLineBuffer,0);
			}
			StripeEnd = -1; //index of the last byte of the stripe
			MinOut=255;MaxOut=0; //initialise

 			if(Page == 1) SetupJob(&header);
			StartPrinterPage( &header);

//prepare for compression JBIG85
			DoLog("initialising Compression\n",0,0);
			jbg85_enc_init(&JbigState, RasForCompWidth * 8, header.cupsHeight, output_jbig, JobFile);
	  		jbg85_enc_options(&JbigState, -1, StripeHeightMax, -1); //default options ie with variable length

// Loop for each source stripe
	   		for (Stripe = 0; Stripe * StripeHeightMax < header.cupsHeight; ++Stripe)  
   			{
				//clear the raster
				for(x=0;x<RasForCompWidth * StripeHeightMax;++x) RasForComp[x]=0;

				if(header.cupsHeight > Stripe * StripeHeightMax + StripeHeightMax) StripeHeight = StripeHeightMax;
				else StripeHeight = header.cupsHeight - (Stripe * StripeHeightMax);
				if(Stripe == 1) DoLog("First stripe at %d sec\n", time(NULL)-StartTime, 0);
		
// Loop for each line in the stripe	
   				for (y = 0; (y < StripeHeightMax) && (y + Stripe * StripeHeightMax < header.cupsHeight); ++y )  
   				{
      					if (Canceled) break;
				 	KeepAwakeStart = KeepAwake(KeepAwakeStart, 10, PrintFile); //Keep the printer connection awake
					if  (header.cupsBitsPerColor == 8) // do dithering
					{
						if(y == 0) 
						{
							DoLog("Doing 1st line in stripe %d. BytesPerLine %d\n", Stripe, header.cupsBytesPerLine);
							if(!ras) DoLog("cups raster missing",0,0);
							if(!CupsLineBuffer) DoLog("cups line buffer missing",0,0);
						}
						//read a line. The longest line 600dpi 8.5 in = 20400 bytes for 8 bit CMYK values
						//DoLog("Reading stripe %d line %d\n", Stripe, y);
  						if (!cupsRasterReadPixels(ras, CupsLineBuffer, header.cupsBytesPerLine)) break; 
						//CupsLineBuffer holds the chunky 8 bit data of a whole line
						if(y == 0) DoLog("Read first line from cups raster\n", Stripe, 0);

#if TESTING == 1
			//fix the CupsLineBuffer for testing with a gradient in the top 32 lines
						if(y + Stripe * StripeHeightMax < 32) InsertGradientChunky(CupsLineBuffer, &header, y);
#endif
// the colour separation. Note the cups transforms assume chunky data
					if (DoTrans == 1)
					{
						RemainingPixels = header.cupsWidth;
						if (header.cupsColorSpace == CUPS_CSPACE_RGB)
						{
							if(y == 0) DoLog("Doing transform & dither (CMYK) stripe %d line %d\n", Stripe, y);
							if(header.HWResolution[0] == 300) cupsCMYKDoRGB(cmykTrans300, CupsLineBuffer, TransformedBuffer, RemainingPixels);
							else cupsCMYKDoRGB(cmykTrans600, CupsLineBuffer, TransformedBuffer, RemainingPixels);
							// cupsDoRGB scales up to 4096 a short int from the unsigned char input 255
							for(Col = 0; Col<4; ++Col) 
							{
								DitherProcess(Lut, DitherState, &header, Col, &BlankColour);
								PutBitsIntoRaster(&header, Col, BlankColour, y);
							}
						}

						if (header.cupsColorSpace == CUPS_CSPACE_K)
						{
							if(y == 0) DoLog("Doing dither (K) stripe %d line %d\n", Stripe, y);
							Col=3; //just black&grey
							DitherProcess(Lut, DitherState, &header, Col, &BlankColour);
							PutBitsIntoRaster(&header, Col, BlankColour, y);
						}

						//DummyTransform(CupsLineBuffer, TransformedBuffer, RemainingPixels);
						if(y == 0) DoLog("Done dither stripe %d line %d\n", Stripe, y);
					}
/*						if(header.cupsColorSpace == CUPS_CSPACE_RGB)
						{
							for(Col = 0; Col<4; ++Col) FakePutBitsIntoRaster(&header, Col, BlankColour, y);
						}
// IS THIS RIGHT? PutBitsIntoRaster(&header, 3? FOR Kk raster?
						else PutBitsIntoRaster(&header, 3, BlankColour, y);
*/						
					} //end of 8 bits per colour section

					else DoLog("cupsBitsPerColor %d is not handled\n",header.cupsBitsPerColor,0);
					
    				} //end of line loop

				StripeEnd = StripeHeight * RasForCompWidth - 1; //so do not have to increment in the loop

				//map 2 bit per pixel data of the line to suit the printer and look for printing in the stripe
				SkipStripe = 1;
				for(i=0;i<=StripeEnd;++i) 
				{
// may not be needed with dither	if (header.cupsBitsPerColor==2) RasForComp[i] = Map11To10And10To01[RasForComp[i]]; //map 2 bit
					if(RasForComp[i]!=0) SkipStripe = 0; //look for printing
				}

#if DEBUGFILES == 1
//write the stripe into the raster files
				if(SkipStripe != 1) 
				{
//store the raster for debugging - does not seem to be viewable properly when 600dpi colour
	    				if (dfp) fwrite(&RasForComp[0], 1, StripeEnd, dfp);
//store the CMYK rasters separately
					if(Cyanfp && Magentafp && Yellowfp && Blackfp && Greyfp) 
					{
						for(i=0;i<(StripeEnd + RasForCompWidth + 1);i=i+RasForCompWidth)
						{
							fwrite(&RasForComp[i + 0 * BytesPerColour], 1,  BytesPerColour, Cyanfp);
							fwrite(&RasForComp[i + 1 * BytesPerColour], 1,  BytesPerColour, Magentafp);
							fwrite(&RasForComp[i + 2 * BytesPerColour], 1,  BytesPerColour, Yellowfp);
							fwrite(&RasForComp[i + 3 * BytesPerColour], 1,  BytesPerColour, Blackfp);
							fwrite(&RasForComp[i + 4 * BytesPerColour], 1,  BytesPerColour, Greyfp);
						}
					}
					else if(Blackfp && Greyfp) 
					{
						for(i=0;i<(StripeEnd + RasForCompWidth + 1);i=i+RasForCompWidth)
						{
							fwrite(&RasForComp[i + 0 * BytesPerColour], 1,  BytesPerColour, Blackfp);
							fwrite(&RasForComp[i + 1 * BytesPerColour], 1,  BytesPerColour, Greyfp);
						}
					}
					DoLog("Rasters stored at %d sec\n",time(NULL)-StartTime,0);
				}
#endif

				//DoLog("StripeEnd=%d SkipStripe=%d\n",StripeEnd,SkipStripe);
				if(!SkipStripe) RasForCompHeight = RasForCompHeight + StripeHeight;

 				PrintOneStripe(&RasForComp[0], Stripe, StripeHeight, header, JobFile);
//copy the last 2 lines of the stripe, before the first line of RasForComp
				for(i=0;i<RasForCompWidth;++i)
				{
					RasForComp[i - RasForCompWidth] = RasForComp[(StripeHeight-1) * RasForCompWidth + i];
					RasForComp[i - RasForCompWidth * 2] = RasForComp[(StripeHeight-2) * RasForCompWidth + i];
				}
   	
				StripeEnd = -1;
				DoLog("sum of stripe pixels %d",StripeAccum,0);
	 		} //end of loop for each stripe

			DoLog("Page raster built at %d sec\n",time(NULL)-StartTime,0);
//page is finished
//all stripes are now done
			DoLog("Max and min in input colour are %d %d\n",MaxIn,MinIn);
			DoLog("Max and min in dithered colour are %d %d\n",MaxOut,MinOut);
			DoLog("Max in cups raster and transformed %d %d\n",CupsLineBufMax,TransformedBufMax);
#if DEBUGFILES == 1
	
//close the debug and dither files
			if(RawColourFile) fclose(RawColourFile);
			if(DitheredColourFile) fclose(DitheredColourFile);
			if(CupsRasterFile) fclose(CupsRasterFile);
			fclose(dfp);
			if(Cyanfp) fclose(Cyanfp);
			if(Magentafp) fclose(Magentafp);
			if(Yellowfp) fclose(Yellowfp);
			fclose(Blackfp);
			fclose(Greyfp);
			sleep(3);

			LetAllRead(RawColourFileName);
			LetAllRead(DitheredColourFileName);
			LetAllRead(RasForCompFileName);
			LetAllRead(CyanRasFileName);
			LetAllRead(MagentaRasFileName);
			LetAllRead(YellowRasFileName);
			LetAllRead(BlackRasFileName);
			LetAllRead(GreyRasFileName);
			LetAllRead(CupsRasterFileName);

#endif
			DoLog("Setting jbig height to %d\n", RasForCompHeight, 0);
			jbg85_enc_newlen(&JbigState, RasForCompHeight);

    			EndPage();
   			if (Canceled)    break;
  		}
		while (cupsRasterReadHeader2(ras, &header) == 1);
   		if (Canceled)    DoLog("Was cancelled",0,0);
	}
	else DoLog("no headers so nothing to print",0,0);

        if (Page > 0) ShutdownPrinter(); //there was at least 1 page

	Terminate(ras,fd,DitherState,Lut);
	return(0);
}
